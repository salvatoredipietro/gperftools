// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2000, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Urs Holzle <opensource@google.com>

#include "config.h"
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <inttypes.h>
// We only need malloc.h for structs mallinfo and mallinfo2.
#if defined(HAVE_STRUCT_MALLINFO) || defined(HAVE_STRUCT_MALLINFO2)
// Malloc can be in several places on older versions of OS X.
# if defined(HAVE_MALLOC_H)
# include <malloc.h>
# elif defined(HAVE_MALLOC_MALLOC_H)
# include <malloc/malloc.h>
# elif defined(HAVE_SYS_MALLOC_H)
# include <sys/malloc.h>
# endif
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <gperftools/malloc_extension.h>
#include <gperftools/malloc_hook.h>

// Will be pulled in as along with tcmalloc.cc
// #include <gperftools/tcmalloc.h>

#include "addressmap-inl.h"
#include "base/commandlineflags.h"
#include "base/googleinit.h"
#include "base/logging.h"
#include "base/spinlock.h"
#include "base/static_storage.h"
#include "base/threading.h"
#include "malloc_backtrace.h"
#include "malloc_hook-inl.h"
#include "maybe_emergency_malloc.h"
#include "safe_strerror.h"
#include "symbolize.h"

// NOTE: due to #define below, tcmalloc.cc will omit tc_XXX
// definitions. So that debug implementations can be defined
// instead. We're going to use do_malloc, do_free and other do_XXX
// functions that are defined in tcmalloc.cc for actual memory
// management
#define TCMALLOC_USING_DEBUGALLOCATION
#include "tcmalloc.cc"

// __THROW is defined in glibc systems.  It means, counter-intuitively,
// "This function will never throw an exception."  It's an optional
// optimization tool, but we may need to use it to match glibc prototypes.
#ifndef __THROW    // I guess we're not on a glibc system
# define __THROW   // __THROW is just an optimization, so ok to make it ""
#endif

// On systems (like freebsd) that don't define MAP_ANONYMOUS, use the old
// form of the name instead.
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

// ========================================================================= //

DEFINE_bool(malloctrace,
            EnvToBool("TCMALLOC_TRACE", false),
            "Enables memory (de)allocation tracing to /tmp/google.alloc.");
#ifdef HAVE_MMAP
DEFINE_bool(malloc_page_fence,
            EnvToBool("TCMALLOC_PAGE_FENCE", false),
            "Enables putting of memory allocations at page boundaries "
            "with a guard page following the allocation (to catch buffer "
            "overruns right when they happen).");
DEFINE_bool(malloc_page_fence_never_reclaim,
            EnvToBool("TCMALLOC_PAGE_FENCE_NEVER_RECLAIM", false),
            "Enables making the virtual address space inaccessible "
            "upon a deallocation instead of returning it and reusing later.");
DEFINE_bool(malloc_page_fence_readable,
            EnvToBool("TCMALLOC_PAGE_FENCE_READABLE", false),
            "Permits reads to the page fence.");
#else
DEFINE_bool(malloc_page_fence, false, "Not usable (requires mmap)");
DEFINE_bool(malloc_page_fence_never_reclaim, false, "Not usable (required mmap)");
#endif
DEFINE_bool(malloc_reclaim_memory,
            EnvToBool("TCMALLOC_RECLAIM_MEMORY", true),
            "If set to false, we never return memory to malloc "
            "when an object is deallocated. This ensures that all "
            "heap object addresses are unique.");
DEFINE_int32(max_free_queue_size,
             EnvToInt("TCMALLOC_MAX_FREE_QUEUE_SIZE", 10*1024*1024),
             "If greater than 0, keep freed blocks in a queue instead of "
             "releasing them to the allocator immediately.  Release them when "
             "the total size of all blocks in the queue would otherwise exceed "
             "this limit.");

DEFINE_bool(symbolize_stacktrace,
            EnvToBool("TCMALLOC_SYMBOLIZE_STACKTRACE", true),
            "Symbolize the stack trace when provided (on some error exits)");

// ========================================================================= //

// A safe version of printf() that does not do any allocation and
// uses very little stack space.
static void TracePrintf(int fd, const char *fmt, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));

// Round "value" up to next "alignment" boundary.
// Requires that "alignment" be a power of two.
static intptr_t RoundUp(intptr_t value, intptr_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// ========================================================================= //

class MallocBlock;

// A circular buffer to hold freed blocks of memory.  MallocBlock::Deallocate
// (below) pushes blocks into this queue instead of returning them to the
// underlying allocator immediately.  See MallocBlock::Deallocate for more
// information.
//
// We can't use an STL class for this because we need to be careful not to
// perform any heap de-allocations in any of the code in this class, since the
// code in MallocBlock::Deallocate is not re-entrant.
template <typename QueueEntry>
class FreeQueue {
 public:
  FreeQueue() : q_front_(0), q_back_(0) {}

  bool Full() {
    return (q_front_ + 1) % kFreeQueueSize == q_back_;
  }

  void Push(const QueueEntry& block) {
    q_[q_front_] = block;
    q_front_ = (q_front_ + 1) % kFreeQueueSize;
  }

  QueueEntry Pop() {
    RAW_CHECK(q_back_ != q_front_, "Queue is empty");
    const QueueEntry& ret = q_[q_back_];
    q_back_ = (q_back_ + 1) % kFreeQueueSize;
    return ret;
  }

  size_t size() const {
    return (q_front_ - q_back_ + kFreeQueueSize) % kFreeQueueSize;
  }

 private:
  // Maximum number of blocks kept in the free queue before being freed.
  static const int kFreeQueueSize = 1024;

  QueueEntry q_[kFreeQueueSize];
  int q_front_;
  int q_back_;
};

struct MallocBlockQueueEntry {
  MallocBlockQueueEntry() : block(nullptr), size(0),
                            num_deleter_pcs(0) {}
  MallocBlockQueueEntry(MallocBlock* b, size_t s) : block(b), size(s) {
    if (FLAGS_max_free_queue_size != 0 && b != nullptr) {
      num_deleter_pcs = tcmalloc::GrabBacktrace(
        deleter_pcs,
        sizeof(deleter_pcs) / sizeof(deleter_pcs[0]),
        1);
      deleter_threadid = tcmalloc::SelfThreadId();
    } else {
      num_deleter_pcs = 0;
    }
  }

  MallocBlock* block;
  size_t size;

  // When deleted and put in the free queue, we (flag-controlled)
  // record the stack so that if corruption is later found, we can
  // print the deleter's stack.  (These three vars add 144 bytes of
  // overhead under the LP64 data model.)
  void* deleter_pcs[16];
  int num_deleter_pcs;
  uintptr_t deleter_threadid;
};

class MallocBlock {
 public:  // allocation type constants

  // Different allocation types we distinguish.
  // Note: The lower 4 bits are not random: we index kAllocName array
  // by these values masked with kAllocTypeMask;
  // the rest are "random" magic bits to help catch memory corruption.
  static const int kMallocType = 0xEFCDAB90;
  static const int kNewType = 0xFEBADC81;
  static const int kArrayNewType = 0xBCEADF72;

 private:  // constants

  // A mask used on alloc types above to get to 0, 1, 2
  static const int kAllocTypeMask = 0x3;
  // An additional bit to set in AllocType constants
  // to mark now deallocated regions.
  static const int kDeallocatedTypeBit = 0x4;

  // For better memory debugging, we initialize all storage to known
  // values, and overwrite the storage when it's deallocated:
  // Byte that fills uninitialized storage.
  static const int kMagicUninitializedByte = 0xAB;
  // Byte that fills deallocated storage.
  // NOTE: tcmalloc.cc depends on the value of kMagicDeletedByte
  //       to work around a bug in the pthread library.
  static const int kMagicDeletedByte = 0xCD;
  // A size_t (type of alloc_type_ below) in a deallocated storage
  // filled with kMagicDeletedByte.
  static const size_t kMagicDeletedSizeT =
      0xCDCDCDCD | (((size_t)0xCDCDCDCD << 16) << 16);
    // Initializer works for 32 and 64 bit size_ts;
    // "<< 16 << 16" is to fool gcc from issuing a warning
    // when size_ts are 32 bits.

  // NOTE: on Linux, you can enable malloc debugging support in libc by
  // setting the environment variable MALLOC_CHECK_ to 1 before you
  // start the program (see man malloc).

  // We use either do_malloc or mmap to make the actual allocation. In
  // order to remember which one of the two was used for any block, we store an
  // appropriate magic word next to the block.
  static const size_t kMagicMalloc = 0xDEADBEEF;
  static const size_t kMagicMMap = 0xABCDEFAB;

  // This array will be filled with 0xCD, for use with memcmp.
  static unsigned char kMagicDeletedBuffer[1024];
  static tcmalloc::TrivialOnce deleted_buffer_initialized_;

 private:  // data layout

                    // The four fields size1_,offset_,magic1_,alloc_type_
                    // should together occupy a multiple of 16 bytes. (At the
                    // moment, sizeof(size_t) == 4 or 8 depending on piii vs
                    // k8, and 4 of those sum to 16 or 32 bytes).
                    // This, combined with do_malloc's alignment guarantees,
                    // ensures that SSE types can be stored into the returned
                    // block, at &size2_.
  size_t size1_;
  size_t offset_;   // normally 0 unless memaligned memory
                    // see comments in memalign() and FromRawPointer().
  size_t magic1_;
  size_t alloc_type_;
  // here comes the actual data (variable length)
  // ...
  // then come the size2_ and magic2_, or a full page of mprotect-ed memory
  // if the malloc_page_fence feature is enabled.
  size_t size_and_magic2_[2];

 private:  // static data and helpers

  // Allocation map: stores the allocation type for each allocated object,
  // or the type or'ed with kDeallocatedTypeBit
  // for each formerly allocated object.
  typedef AddressMap<int> AllocMap;
  static inline AllocMap* alloc_map_;
  // This protects alloc_map_ and consistent state of metadata
  // for each still-allocated object in it.
  // We use spin locks instead of pthread_mutex_t locks
  // to prevent crashes via calls to pthread_mutex_(un)lock
  // for the (de)allocations coming from pthreads initialization itself.
  static inline SpinLock alloc_map_lock_;

  // A queue of freed blocks.  Instead of releasing blocks to the allocator
  // immediately, we put them in a queue, freeing them only when necessary
  // to keep the total size of all the freed blocks below the limit set by
  // FLAGS_max_free_queue_size.
  static inline FreeQueue<MallocBlockQueueEntry>* free_queue_;

  static inline size_t free_queue_size_;  // total size of blocks in free_queue_
  // protects free_queue_ and free_queue_size_
  static inline SpinLock free_queue_lock_;

  // Names of allocation types (kMallocType, kNewType, kArrayNewType)
  static const char* const kAllocName[];
  // Names of corresponding deallocation types
  static const char* const kDeallocName[];

  static const char* AllocName(int type) {
    return kAllocName[type & kAllocTypeMask];
  }

  static const char* DeallocName(int type) {
    return kDeallocName[type & kAllocTypeMask];
  }

 private:  // helper accessors

  bool IsMMapped() const { return kMagicMMap == magic1_; }

  bool IsValidMagicValue(size_t value) const {
    return kMagicMMap == value  ||  kMagicMalloc == value;
  }

  static size_t real_malloced_size(size_t size) {
    return size + sizeof(MallocBlock);
  }

  /*
   * Here we assume size of page is kMinAlign aligned,
   * so if size is MALLOC_ALIGNMENT aligned too, then we could
   * guarantee return address is also kMinAlign aligned, because
   * mmap return address at nearby page boundary on Linux.
   */
  static size_t real_mmapped_size(size_t size) {
    size_t tmp = size + MallocBlock::data_offset();
    tmp = RoundUp(tmp, kMinAlign);
    return tmp;
  }

  size_t real_size() {
    return IsMMapped() ? real_mmapped_size(size1_) : real_malloced_size(size1_);
  }

  // NOTE: if the block is mmapped (that is, we're using the
  // malloc_page_fence option) then there's no size2 or magic2
  // (instead, the guard page begins where size2 would be).

  const size_t* size2_addr() const {
    return (const size_t*)((const char*)&size_and_magic2_ + size1_);
  }
  size_t* size2_addr() {
    const auto* cthis = this;
    return const_cast<size_t*>(cthis->size2_addr());
  }

  size_t* magic2_addr() { return (size_t*)(size2_addr() + 1); }
  const size_t* magic2_addr() const { return (const size_t*)(size2_addr() + 1); }

 private:  // other helpers

  void Initialize(size_t size, int type) {
    RAW_CHECK(IsValidMagicValue(magic1_), "");
    // record us as allocated in the map
    alloc_map_lock_.Lock();
    if (!alloc_map_) {
      void* p = do_malloc(sizeof(AllocMap));
      alloc_map_ = new(p) AllocMap(do_malloc, do_free);
    }
    alloc_map_->Insert(data_addr(), type);
    // initialize us
    size1_ = size;
    offset_ = 0;
    alloc_type_ = type;
    if (!IsMMapped()) {
      bit_store(magic2_addr(), &magic1_);
      bit_store(size2_addr(), &size);
    }
    alloc_map_lock_.Unlock();
    memset(data_addr(), kMagicUninitializedByte, size);
    if (!IsMMapped()) {
      RAW_CHECK(memcmp(&size1_, size2_addr(), sizeof(size1_)) == 0, "should hold");
      RAW_CHECK(memcmp(&magic1_, magic2_addr(), sizeof(magic1_)) == 0, "should hold");
    }
  }

  size_t CheckAndClear(int type, size_t given_size) {
    alloc_map_lock_.Lock();
    CheckLocked(type);
    if (!IsMMapped()) {
      RAW_CHECK(memcmp(&size1_, size2_addr(), sizeof(size1_)) == 0, "should hold");
    }
    // record us as deallocated in the map
    alloc_map_->Insert(data_addr(), type | kDeallocatedTypeBit);
    alloc_map_lock_.Unlock();
    // clear us
    const size_t size = real_size();
#if !defined(TCMALLOC_DONT_VERIFY_SIZE)
    RAW_CHECK(!given_size || given_size == size1_,
              "right size must be passed to sized delete");
#endif
    memset(this, kMagicDeletedByte, size);
    return size;
  }

  void CheckLocked(int type) const {
    int map_type = 0;
    const int* found_type =
      alloc_map_ != nullptr ? alloc_map_->Find(data_addr()) : nullptr;
    if (found_type == nullptr) {
      RAW_LOG(FATAL, "memory allocation bug: object at %p "
                     "has never been allocated", data_addr());
    } else {
      map_type = *found_type;
    }
    if ((map_type & kDeallocatedTypeBit) != 0) {
      RAW_LOG(FATAL, "memory allocation bug: object at %p "
                     "has been already deallocated (it was allocated with %s)",
                     data_addr(), AllocName(map_type & ~kDeallocatedTypeBit));
    }
    if (alloc_type_ == kMagicDeletedSizeT) {
      RAW_LOG(FATAL, "memory stomping bug: a word before object at %p "
                     "has been corrupted; or else the object has been already "
                     "deallocated and our memory map has been corrupted",
                     data_addr());
    }
    if (!IsValidMagicValue(magic1_)) {
      RAW_LOG(FATAL, "memory stomping bug: a word before object at %p "
                     "has been corrupted; "
                     "or else our memory map has been corrupted and this is a "
                     "deallocation for not (currently) heap-allocated object",
                     data_addr());
    }
    if (!IsMMapped()) {
      if (memcmp(&size1_, size2_addr(), sizeof(size1_))) {
        RAW_LOG(FATAL, "memory stomping bug: a word after object at %p "
                       "has been corrupted", data_addr());
      }
      size_t addr;
      bit_store(&addr, magic2_addr());
      if (!IsValidMagicValue(addr)) {
        RAW_LOG(FATAL, "memory stomping bug: a word after object at %p "
                "has been corrupted", data_addr());
      }
    }
    if (alloc_type_ != type) {
      if ((alloc_type_ != MallocBlock::kMallocType) &&
          (alloc_type_ != MallocBlock::kNewType)    &&
          (alloc_type_ != MallocBlock::kArrayNewType)) {
        RAW_LOG(FATAL, "memory stomping bug: a word before object at %p "
                       "has been corrupted", data_addr());
      }
      RAW_LOG(FATAL, "memory allocation/deallocation mismatch at %p: "
                     "allocated with %s being deallocated with %s",
                     data_addr(), AllocName(alloc_type_), DeallocName(type));
    }
    if (alloc_type_ != map_type) {
      RAW_LOG(FATAL, "memory stomping bug: our memory map has been corrupted : "
                     "allocation at %p made with %s "
                     "is recorded in the map to be made with %s",
                     data_addr(), AllocName(alloc_type_),  AllocName(map_type));
    }
  }

 public:  // public accessors

  void* data_addr() { return (void*)&size_and_magic2_; }
  const void* data_addr() const { return (const void*)&size_and_magic2_; }

  static size_t data_offset() { return OFFSETOF_MEMBER(MallocBlock, size_and_magic2_); }

  size_t raw_data_size() const { return size1_; }

  // Note, this allocation might actually be from memalign, so raw_ptr
  // might be >= data_addr() (see FromRawPointer and do_debug_memalign
  // for how it works). So in order to get data size we should be
  // careful.
  size_t actual_data_size(const void* raw_ptr) const {
    const char* raw_begin = static_cast<const char*>(data_addr());
    const char* raw_end = raw_begin + raw_data_size();
    CHECK_CONDITION(raw_begin <= raw_end);
    CHECK_CONDITION(raw_begin <= raw_ptr);
    CHECK_CONDITION(raw_ptr <= raw_end);

    return raw_end - static_cast<const char*>(raw_ptr);
  }

  void set_offset(int offset) { this->offset_ = offset; }

 public:  // our main interface

  static MallocBlock* Allocate(size_t size, int type) {
    // Prevent an integer overflow / crash with large allocation sizes.
    // TODO - Note that for a e.g. 64-bit size_t, max_size_t may not actually
    // be the maximum value, depending on how the compiler treats ~0. The worst
    // practical effect is that allocations are limited to 4Gb or so, even if
    // the address space could take more.
    static size_t max_size_t = ~0;
    if (size > max_size_t - sizeof(MallocBlock)) {
      RAW_LOG(ERROR, "Massive size passed to malloc: %zu", size);
      return nullptr;
    }
    MallocBlock* b = nullptr;
    const bool use_malloc_page_fence = FLAGS_malloc_page_fence;
    const bool malloc_page_fence_readable = FLAGS_malloc_page_fence_readable;
#ifdef HAVE_MMAP
    if (use_malloc_page_fence) {
      // Put the block towards the end of the page and make the next page
      // inaccessible. This will catch buffer overrun right when it happens.
      size_t sz = real_mmapped_size(size);
      int pagesize = getpagesize();
      int num_pages = (sz + pagesize - 1) / pagesize + 1;
      char* p = (char*) mmap(nullptr, num_pages * pagesize, PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
      if (p == MAP_FAILED) {
        // If the allocation fails, abort rather than returning nullptr to
        // malloc. This is because in most cases, the program will run out
        // of memory in this mode due to tremendous amount of wastage. There
        // is no point in propagating the error elsewhere.
        RAW_LOG(FATAL, "Out of memory: possibly due to page fence overhead: %s",
                tcmalloc::SafeStrError(errno).c_str());
      }
      // Mark the page after the block inaccessible
      if (mprotect(p + (num_pages - 1) * pagesize, pagesize,
                   PROT_NONE|(malloc_page_fence_readable ? PROT_READ : 0))) {
        RAW_LOG(FATAL, "Guard page setup failed: %s",
                tcmalloc::SafeStrError(errno).c_str());
      }
      b = (MallocBlock*) (p + (num_pages - 1) * pagesize - sz);
    } else {
      b = (MallocBlock*) do_malloc(real_malloced_size(size));
    }
#else
    b = (MallocBlock*) do_malloc(real_malloced_size(size));
#endif

    // It would be nice to output a diagnostic on allocation failure
    // here, but logging (other than FATAL) requires allocating
    // memory, which could trigger a nasty recursion. Instead, preserve
    // malloc semantics and return nullptr on failure.
    if (b != nullptr) {
      b->magic1_ = use_malloc_page_fence ? kMagicMMap : kMagicMalloc;
      b->Initialize(size, type);
    }
    return b;
  }

  void Deallocate(int type, size_t given_size) {
    if (IsMMapped()) {  // have to do this before CheckAndClear
#ifdef HAVE_MMAP
      int size = CheckAndClear(type, given_size);
      int pagesize = getpagesize();
      int num_pages = (size + pagesize - 1) / pagesize + 1;
      char* p = (char*) this;
      if (FLAGS_malloc_page_fence_never_reclaim  ||
          !FLAGS_malloc_reclaim_memory) {
        mprotect(p - (num_pages - 1) * pagesize + size,
                 num_pages * pagesize, PROT_NONE);
      } else {
        munmap(p - (num_pages - 1) * pagesize + size, num_pages * pagesize);
      }
#endif
    } else {
      const size_t size = CheckAndClear(type, given_size);
      if (FLAGS_malloc_reclaim_memory) {
        // Instead of freeing the block immediately, push it onto a queue of
        // recently freed blocks.  Free only enough blocks to keep from
        // exceeding the capacity of the queue or causing the total amount of
        // un-released memory in the queue from exceeding
        // FLAGS_max_free_queue_size.
        ProcessFreeQueue(this, size, FLAGS_max_free_queue_size);
      }
    }
  }

  static size_t FreeQueueSize() {
    SpinLockHolder l(&free_queue_lock_);
    return free_queue_size_;
  }

  static void ProcessFreeQueue(MallocBlock* b, size_t size,
                               int max_free_queue_size) {
    // MallocBlockQueueEntry are about 144 in size, so we can only
    // use a small array of them on the stack.
    MallocBlockQueueEntry entries[4];
    int num_entries = 0;
    MallocBlockQueueEntry new_entry(b, size);
    free_queue_lock_.Lock();
    if (free_queue_ == nullptr)
      free_queue_ = new FreeQueue<MallocBlockQueueEntry>;
    RAW_CHECK(!free_queue_->Full(), "Free queue mustn't be full!");

    if (b != nullptr) {
      free_queue_size_ += size + sizeof(MallocBlockQueueEntry);
      free_queue_->Push(new_entry);
    }

    // Free blocks until the total size of unfreed blocks no longer exceeds
    // max_free_queue_size, and the free queue has at least one free
    // space in it.
    while (free_queue_size_ > max_free_queue_size || free_queue_->Full()) {
      RAW_CHECK(num_entries < arraysize(entries), "entries array overflow");
      entries[num_entries] = free_queue_->Pop();
      free_queue_size_ -=
          entries[num_entries].size + sizeof(MallocBlockQueueEntry);
      num_entries++;
      if (num_entries == arraysize(entries)) {
        // The queue will not be full at this point, so it is ok to
        // release the lock.  The queue may still contain more than
        // max_free_queue_size, but this is not a strict invariant.
        free_queue_lock_.Unlock();
        for (int i = 0; i < num_entries; i++) {
          CheckForDanglingWrites(entries[i]);
          do_free(entries[i].block);
        }
        num_entries = 0;
        free_queue_lock_.Lock();
      }
    }
    free_queue_lock_.Unlock();
    for (int i = 0; i < num_entries; i++) {
      CheckForDanglingWrites(entries[i]);
      do_free(entries[i].block);
    }
  }

  static void InitDeletedBuffer() {
    memset(kMagicDeletedBuffer, kMagicDeletedByte, sizeof(kMagicDeletedBuffer));
  }

  static void CheckForDanglingWrites(const MallocBlockQueueEntry& queue_entry) {
    // Initialize the buffer if necessary.
    deleted_buffer_initialized_.RunOnce(&InitDeletedBuffer);

    const unsigned char* p =
        reinterpret_cast<unsigned char*>(queue_entry.block);

    static const size_t size_of_buffer = sizeof(kMagicDeletedBuffer);
    const size_t size = queue_entry.size;
    const size_t buffers = size / size_of_buffer;
    const size_t remainder = size % size_of_buffer;
    size_t buffer_idx;
    for (buffer_idx = 0; buffer_idx < buffers; ++buffer_idx) {
      CheckForCorruptedBuffer(queue_entry, buffer_idx, p, size_of_buffer);
      p += size_of_buffer;
    }
    CheckForCorruptedBuffer(queue_entry, buffer_idx, p, remainder);
  }

  static void CheckForCorruptedBuffer(const MallocBlockQueueEntry& queue_entry,
                                      size_t buffer_idx,
                                      const unsigned char* buffer,
                                      size_t size_of_buffer) {
    if (memcmp(buffer, kMagicDeletedBuffer, size_of_buffer) == 0) {
      return;
    }

    RAW_LOG(ERROR,
            "Found a corrupted memory buffer in MallocBlock (may be offset "
            "from user ptr): buffer index: %zd, buffer ptr: %p, size of "
            "buffer: %zd", buffer_idx, buffer, size_of_buffer);

    // The magic deleted buffer should only be 1024 bytes, but in case
    // this changes, let's put an upper limit on the number of debug
    // lines we'll output:
    if (size_of_buffer <= 1024) {
      for (int i = 0; i < size_of_buffer; ++i) {
        if (buffer[i] != kMagicDeletedByte) {
          RAW_LOG(ERROR, "Buffer byte %d is 0x%02x (should be 0x%02x).",
                  i, buffer[i], kMagicDeletedByte);
        }
      }
    } else {
      RAW_LOG(ERROR, "Buffer too large to print corruption.");
    }

    const MallocBlock* b = queue_entry.block;
    const size_t size = queue_entry.size;
    if (queue_entry.num_deleter_pcs > 0) {
      TracePrintf(STDERR_FILENO, "Deleted by thread %zx\n",
                  queue_entry.deleter_threadid);

      ThreadCachePtr::WithStacktraceScope([&] (bool stacktrace_allowed) {
        tcmalloc::DumpStackTraceToStderr(queue_entry.deleter_pcs, queue_entry.num_deleter_pcs,
                                         FLAGS_symbolize_stacktrace,
                                         "    @ ");
      });
    } else {
      RAW_LOG(ERROR,
              "Skipping the printing of the deleter's stack!  Its stack was "
              "not found; either the corruption occurred too early in "
              "execution to obtain a stack trace or --max_free_queue_size was "
              "set to 0.");
    }

    RAW_LOG(FATAL,
            "Memory was written to after being freed.  MallocBlock: %p, user "
            "ptr: %p, size: %zd.  If you can't find the source of the error, "
            "try using ASan (https://github.com/google/sanitizers), "
            "Valgrind, or Purify, or study the "
            "output of the deleter's stack printed above.",
            b, b->data_addr(), size);
  }

  static MallocBlock* FromRawPointer(void* p) {
    const size_t data_offset = MallocBlock::data_offset();
    // Find the header just before client's memory.
    MallocBlock *mb = reinterpret_cast<MallocBlock *>(
                reinterpret_cast<char *>(p) - data_offset);
    // If mb->alloc_type_ is kMagicDeletedSizeT, we're not an ok pointer.
    if (mb->alloc_type_ == kMagicDeletedSizeT) {
      RAW_LOG(FATAL, "memory allocation bug: object at %p has been already"
                     " deallocated; or else a word before the object has been"
                     " corrupted (memory stomping bug)", p);
    }
    // If mb->offset_ is zero (common case), mb is the real header.
    // If mb->offset_ is non-zero, this block was allocated by debug
    // memallign implementation, and mb->offset_ is the distance
    // backwards to the real header from mb, which is a fake header.
    if (mb->offset_ == 0) {
      return mb;
    }

    MallocBlock *main_block = reinterpret_cast<MallocBlock *>(
      reinterpret_cast<char *>(mb) - mb->offset_);

    if (main_block->offset_ != 0) {
      RAW_LOG(FATAL, "memory corruption bug: offset_ field is corrupted."
              " Need 0 but got %x",
              (unsigned)(main_block->offset_));
    }
    if (main_block >= p) {
      RAW_LOG(FATAL, "memory corruption bug: offset_ field is corrupted."
              " Detected main_block address overflow: %x",
              (unsigned)(mb->offset_));
    }
    if (main_block->size2_addr() < p) {
      RAW_LOG(FATAL, "memory corruption bug: offset_ field is corrupted."
              " It points below it's own main_block: %x",
              (unsigned)(mb->offset_));
    }

    return main_block;
  }

  static const MallocBlock* FromRawPointer(const void* p) {
    // const-safe version: we just cast about
    return FromRawPointer(const_cast<void*>(p));
  }

  void Check(int type) const {
    alloc_map_lock_.Lock();
    CheckLocked(type);
    alloc_map_lock_.Unlock();
  }

  static bool CheckEverything() {
    alloc_map_lock_.Lock();
    if (alloc_map_) {
      alloc_map_->Iterate([] (const void* ptr, int* type) {
        if ((*type & kDeallocatedTypeBit) == 0) {
          FromRawPointer(ptr)->CheckLocked(*type);
        }
      });
    }
    alloc_map_lock_.Unlock();
    return true;  // if we get here, we're okay
  }

  static bool MemoryStats(int* blocks, size_t* total,
                          int histogram[kMallocHistogramSize]) {
    memset(histogram, 0, kMallocHistogramSize * sizeof(int));
    alloc_map_lock_.Lock();
    stats_blocks_ = 0;
    stats_total_ = 0;
    stats_histogram_ = histogram;

    if (alloc_map_) {
      alloc_map_->Iterate([] (const void* ptr, int* type) {
        if ((*type & kDeallocatedTypeBit) == 0) {
          const MallocBlock* b = FromRawPointer(ptr);
          b->CheckLocked(*type);
          ++stats_blocks_;
          size_t mysize = b->size1_;
          int entry = 0;
          stats_total_ += mysize;
          while (mysize) {
            ++entry;
            mysize >>= 1;
          }
          RAW_CHECK(entry < kMallocHistogramSize,
                    "kMallocHistogramSize should be at least as large as log2 "
                    "of the maximum process memory size");
          stats_histogram_[entry] += 1;
        }
      });
    }

    *blocks = stats_blocks_;
    *total = stats_total_;
    alloc_map_lock_.Unlock();
    return true;
  }

 private:  // helpers for CheckEverything and MemoryStats

  // Accumulation variables for StatsCallback protected by alloc_map_lock_
  static int stats_blocks_;
  static size_t stats_total_;
  static int* stats_histogram_;
};

void DanglingWriteChecker() {
  // Clear out the remaining free queue to check for dangling writes.
  MallocBlock::ProcessFreeQueue(nullptr, 0, 0);
}

// ========================================================================= //

const size_t MallocBlock::kMagicMalloc;
const size_t MallocBlock::kMagicMMap;

unsigned char MallocBlock::kMagicDeletedBuffer[1024];
tcmalloc::TrivialOnce MallocBlock::deleted_buffer_initialized_;

const char* const MallocBlock::kAllocName[] = {
  "malloc",
  "new",
  "new []",
  nullptr,
};

const char* const MallocBlock::kDeallocName[] = {
  "free",
  "delete",
  "delete []",
  nullptr,
};

int MallocBlock::stats_blocks_;
size_t MallocBlock::stats_total_;
int* MallocBlock::stats_histogram_;

// ========================================================================= //

// The following cut-down version of printf() avoids
// using stdio or ostreams.
// This is to guarantee no recursive calls into
// the allocator and to bound the stack space consumed.  (The pthread
// manager thread in linuxthreads has a very small stack,
// so fprintf can't be called.)
static void TracePrintf(int fd, const char *fmt, ...) {
  char buf[64];
  int i = 0;
  va_list ap;
  va_start(ap, fmt);
  const char *p = fmt;
  char numbuf[25];
  if (fd < 0) {
    va_end(ap);
    return;
  }
  numbuf[sizeof(numbuf)-1] = 0;
  while (*p != '\0') {              // until end of format string
    char *s = &numbuf[sizeof(numbuf)-1];
    if (p[0] == '%' && p[1] != 0) {  // handle % formats
      int64_t l = 0;
      unsigned long base = 0;
      if (*++p == 's') {                            // %s
        s = va_arg(ap, char *);
      } else if (*p == 'l' && p[1] == 'd') {        // %ld
        l = va_arg(ap, long);
        base = 10;
        p++;
      } else if (*p == 'l' && p[1] == 'u') {        // %lu
        l = va_arg(ap, unsigned long);
        base = 10;
        p++;
      } else if (*p == 'z' && p[1] == 'u') {        // %zu
        l = va_arg(ap, size_t);
        base = 10;
        p++;
      } else if (*p == 'z' && p[1] == 'x') {        // %zx
        l = va_arg(ap, size_t);
        base = 16;
        p++;
      } else if (*p == 'u') {                       // %u
        l = va_arg(ap, unsigned int);
        base = 10;
      } else if (*p == 'd') {                       // %d
        l = va_arg(ap, int);
        base = 10;
      } else if (*p == 'p') {                       // %p
        l = va_arg(ap, intptr_t);
        base = 16;
      } else {
        WRITE_TO_STDERR("Unimplemented TracePrintf format\n", 33);
        WRITE_TO_STDERR(p, 2);
        WRITE_TO_STDERR("\n", 1);
        abort();
      }
      p++;
      if (base != 0) {
        bool minus = (l < 0 && base == 10);
        uint64_t ul = minus? -l : l;
        do {
          *--s = "0123456789abcdef"[ul % base];
          ul /= base;
        } while (ul != 0);
        if (base == 16) {
          *--s = 'x';
          *--s = '0';
        } else if (minus) {
          *--s = '-';
        }
      }
    } else {                        // handle normal characters
      *--s = *p++;
    }
    while (*s != 0) {
      if (i == sizeof(buf)) {
        auto unused = write(fd, buf, i);
        (void)unused;
        i = 0;
      }
      buf[i++] = *s++;
    }
  }
  if (i != 0) {
    auto unused = write(fd, buf, i);
    (void)unused;
  }
  va_end(ap);
}

// Return the file descriptor we're writing a log to
static int TraceFd() {
  static int trace_fd = -1;
  if (trace_fd == -1) {            // Open the trace file on the first call
    const char *val = getenv("TCMALLOC_TRACE_FILE");
    bool fallback_to_stderr = false;
    if (!val) {
      val = "/tmp/google.alloc";
      fallback_to_stderr = true;
    }
    trace_fd = open(val, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (trace_fd == -1) {
      if (fallback_to_stderr) {
        trace_fd = 2;
        TracePrintf(trace_fd, "Can't open %s.  Logging to stderr.\n", val);
      } else {
        TracePrintf(2, "Can't open %s.  Logging disabled.\n", val);
      }
    }
    // Add a header to the log.
    TracePrintf(trace_fd, "Trace started: %lu\n",
                static_cast<unsigned long>(time(nullptr)));
    TracePrintf(trace_fd,
                "func\tsize\tptr\tthread_id\tstack pcs for tools/symbolize\n");
  }
  return trace_fd;
}

// Print the hex stack dump on a single line.   PCs are separated by tabs.
static void TraceStack(void) {
  void *pcs[16];
  int n = tcmalloc::GrabBacktrace(pcs, sizeof(pcs)/sizeof(pcs[0]), 0);
  for (int i = 0; i != n; i++) {
    TracePrintf(TraceFd(), "\t%p", pcs[i]);
  }
}

// This protects MALLOC_TRACE, to make sure its info is atomically written.
static SpinLock malloc_trace_lock;

#define MALLOC_TRACE(name, size, addr)                                               \
  do {                                                                               \
    if (FLAGS_malloctrace) {                                                         \
      SpinLockHolder l(&malloc_trace_lock);                                          \
      TracePrintf(TraceFd(), "%s\t%zu\t%p\t%zu",                                     \
                  name, size, addr, tcmalloc::SelfThreadId());                       \
      TraceStack();                                                                  \
      TracePrintf(TraceFd(), "\n");                                                  \
    }                                                                                \
  } while (0)

// ========================================================================= //

// Write the characters buf[0, ..., size-1] to
// the malloc trace buffer.
// This function is intended for debugging,
// and is not declared in any header file.
// You must insert a declaration of it by hand when you need
// to use it.
void __malloctrace_write(const char *buf, size_t size) {
  if (FLAGS_malloctrace) {
    auto unused = write(TraceFd(), buf, size);
    (void)unused;
  }
}

// ========================================================================= //

// General debug allocation/deallocation

static inline void* DebugAllocate(size_t size, int type) {
  if (PREDICT_FALSE(tcmalloc::ThreadCachePtr::Grab().IsEmergencyMallocEnabled())) {
    return tcmalloc::EmergencyMalloc(size);
  }

#if defined(__APPLE__)
  // OSX malloc zones integration has some odd behavior. When
  // GetAllocatedSize returns 0 it appears to assume something wrong
  // about the pointer. And since in debug allocator we can return 0
  // if original size was also 0, lets avoid this case. But only on
  // OSX. It weakens debug checks a bit, but it unbreaks some tests
  // (around realloc/free of 0-sized chunks).
  if (size == 0) size = 1;
#endif
  MallocBlock* ptr = MallocBlock::Allocate(size, type);
  if (ptr == nullptr)  return nullptr;
  MALLOC_TRACE("malloc", size, ptr->data_addr());
  return ptr->data_addr();
}

static inline void DebugDeallocate(void* ptr, int type, size_t given_size) {
  if (PREDICT_FALSE(tcmalloc::IsEmergencyPtr(ptr))) {
    return tcmalloc::EmergencyFree(ptr);
  }

  MALLOC_TRACE("free",
               (ptr != 0 ? MallocBlock::FromRawPointer(ptr)->actual_data_size(ptr) : 0),
               ptr);
  if (ptr)  MallocBlock::FromRawPointer(ptr)->Deallocate(type, given_size);
}

class ATTRIBUTE_VISIBILITY_HIDDEN DebugTestingPortal : public TestingPortalImpl {
public:
  ~DebugTestingPortal() override = default;
  bool IsDebuggingMalloc() override { return true; }
  int32_t& GetMaxFreeQueueSize() override { return FLAGS_max_free_queue_size; }
};

// ========================================================================= //

// The following functions may be called via MallocExtension::instance()
// for memory verification and statistics.
class DebugMallocImplementation : public TCMallocImplementation {
 public:
  virtual bool GetNumericProperty(const char* name, size_t* value) {
    if (TestingPortal** portal = TestingPortal::CheckGetPortal(name, value); portal) {
      static DebugTestingPortal* ptr = ([] () {
        static tcmalloc::StaticStorage<DebugTestingPortal> storage;
        return storage.Construct();
      })();
      *portal = ptr;
      *value = 1;
      return true;
    }

    bool result = TCMallocImplementation::GetNumericProperty(name, value);
    if (result && (strcmp(name, "generic.current_allocated_bytes") == 0)) {
      // Subtract bytes kept in the free queue
      size_t qsize = MallocBlock::FreeQueueSize();
      if (*value >= qsize) {
        *value -= qsize;
      }
    }
    return result;
  }

  virtual bool VerifyNewMemory(const void* p) {
    if (p)  MallocBlock::FromRawPointer(p)->Check(MallocBlock::kNewType);
    return true;
  }

  virtual bool VerifyArrayNewMemory(const void* p) {
    if (p)  MallocBlock::FromRawPointer(p)->Check(MallocBlock::kArrayNewType);
    return true;
  }

  virtual bool VerifyMallocMemory(const void* p) {
    if (p)  MallocBlock::FromRawPointer(p)->Check(MallocBlock::kMallocType);
    return true;
  }

  virtual bool VerifyAllMemory() {
    return MallocBlock::CheckEverything();
  }

  virtual bool MallocMemoryStats(int* blocks, size_t* total,
                                 int histogram[kMallocHistogramSize]) {
    return MallocBlock::MemoryStats(blocks, total, histogram);
  }

  virtual size_t GetEstimatedAllocatedSize(size_t size) {
    return size;
  }

  virtual size_t GetAllocatedSize(const void* p) {
    if (p) {
      RAW_CHECK(GetOwnership(p) != MallocExtension::kNotOwned,
                "ptr not allocated by tcmalloc");
      if (tcmalloc::IsEmergencyPtr(p)) {
        return tcmalloc::EmergencyAllocatedSize(p);
      }
      return MallocBlock::FromRawPointer(p)->actual_data_size(p);
    }
    return 0;
  }

  virtual MallocExtension::Ownership GetOwnership(const void* p) {
    if (!p) {
      // nobody owns nullptr
      return MallocExtension::kNotOwned;
    }

    // FIXME: note that correct GetOwnership should not touch memory
    // that is not owned by tcmalloc. Main implementation is using
    // pagemap to discover if page in question is owned by us or
    // not. But pagemap only has marks for first and last page of
    // spans.  Note that if p was returned out of our memalign with
    // big alignment, then it will point outside of marked pages. Also
    // note that FromRawPointer call below requires touching memory
    // before pointer in order to handle memalign-ed chunks
    // (offset_). This leaves us with two options:
    //
    // * do FromRawPointer first and have possibility of crashing if
    //   we're given not owned pointer
    //
    // * return incorrect ownership for those large memalign chunks
    //
    // I've decided to choose later, which appears to happen rarer and
    // therefore is arguably a lesser evil

    MallocExtension::Ownership rv = TCMallocImplementation::GetOwnership(p);
    if (rv != MallocExtension::kOwned) {
      return rv;
    }

    if (tcmalloc::IsEmergencyPtr(p)) {
      return kOwned;
    }

    const MallocBlock* mb = MallocBlock::FromRawPointer(p);
    return TCMallocImplementation::GetOwnership(mb);
  }

  virtual void GetFreeListSizes(std::vector<MallocExtension::FreeListInfo>* v) {
    static const char* kDebugFreeQueue = "debug.free_queue";

    TCMallocImplementation::GetFreeListSizes(v);

    MallocExtension::FreeListInfo i;
    i.type = kDebugFreeQueue;
    i.min_object_size = 0;
    i.max_object_size = std::numeric_limits<size_t>::max();
    i.total_bytes_free = MallocBlock::FreeQueueSize();
    v->push_back(i);
  }

};

static tcmalloc::StaticStorage<DebugMallocImplementation> debug_malloc_impl_storage;

void SetupMallocExtension() {
  MallocExtension::Register(debug_malloc_impl_storage.Construct());
}

REGISTER_MODULE_DESTRUCTOR(debugallocation, {
  if (!RunningOnValgrind()) {
    // When the program exits, check all blocks still in the free
    // queue for corruption.
    DanglingWriteChecker();
  }
});

// ========================================================================= //

struct debug_alloc_retry_data {
  size_t size;
  int new_type;
};

static void *retry_debug_allocate(void *arg) {
  debug_alloc_retry_data *data = static_cast<debug_alloc_retry_data *>(arg);
  return DebugAllocate(data->size, data->new_type);
}

// This is mostly the same a cpp_alloc in tcmalloc.cc.
// TODO(csilvers): change Allocate() above to call cpp_alloc, so we
// don't have to reproduce the logic here.  To make tc_new_mode work
// properly, I think we'll need to separate out the logic of throwing
// from the logic of calling the new-handler.
inline void* debug_cpp_alloc(size_t size, int new_type, bool nothrow) {
  void* p = DebugAllocate(size, new_type);
  if (p != nullptr) {
    return p;
  }
  struct debug_alloc_retry_data data;
  data.size = size;
  data.new_type = new_type;
  return handle_oom(retry_debug_allocate, &data,
                    true, nothrow);
}

inline void* do_debug_malloc_or_debug_cpp_alloc(size_t size) {
  void* p = DebugAllocate(size, MallocBlock::kMallocType);
  if (p != nullptr) {
    return p;
  }
  struct debug_alloc_retry_data data;
  data.size = size;
  data.new_type = MallocBlock::kMallocType;
  return handle_oom(retry_debug_allocate, &data,
                    false, true);
}

// Exported routines

// frame forcer and force_frame exist only to prevent tail calls to
// DebugDeallocate to be actually implemented as tail calls. This is
// important because stack trace capturing in MallocBlockQueueEntry
// relies on google_malloc section being on stack and tc_XXX functions
// are in that section. So they must not jump to DebugDeallocate but
// have to do call. frame_forcer call at the end of such functions
// prevents tail calls to DebugDeallocate.
static int frame_forcer;
static void force_frame() {
  int dummy = *(int volatile *)&frame_forcer;
  (void)dummy;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_malloc(size_t size) PERFTOOLS_NOTHROW {
  void* ptr = do_debug_malloc_or_debug_cpp_alloc(size);
  tcmalloc::InvokeNewHook(ptr, size);
  return ptr;
}

extern "C" PERFTOOLS_DLL_DECL void tc_free(void* ptr) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(ptr);
  DebugDeallocate(ptr, MallocBlock::kMallocType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void tc_free_sized(void *ptr, size_t size) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(ptr);
  DebugDeallocate(ptr, MallocBlock::kMallocType, size);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void tc_free_aligned_sized(void* ptr, size_t align, size_t size) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(ptr);
  DebugDeallocate(ptr, MallocBlock::kMallocType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void* tc_calloc(size_t count, size_t size) PERFTOOLS_NOTHROW {
  // Overflow check
  const size_t total_size = count * size;
  if (size != 0 && total_size / size != count) return nullptr;

  void* block = do_debug_malloc_or_debug_cpp_alloc(total_size);
  if (block)  memset(block, 0, total_size);
  tcmalloc::InvokeNewHook(block, total_size);
  return block;
}

extern "C" PERFTOOLS_DLL_DECL void tc_cfree(void* ptr) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(ptr);
  DebugDeallocate(ptr, MallocBlock::kMallocType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void* tc_realloc(void* ptr, size_t size) PERFTOOLS_NOTHROW {
  if (ptr == nullptr) {
    ptr = do_debug_malloc_or_debug_cpp_alloc(size);
    tcmalloc::InvokeNewHook(ptr, size);
    return ptr;
  }
  if (size == 0) {
    tcmalloc::InvokeDeleteHook(ptr);
    DebugDeallocate(ptr, MallocBlock::kMallocType, 0);
    return nullptr;
  }

  if (PREDICT_FALSE(tcmalloc::IsEmergencyPtr(ptr))) {
    return tcmalloc::EmergencyRealloc(ptr, size);
  }

  MallocBlock* old = MallocBlock::FromRawPointer(ptr);
  old->Check(MallocBlock::kMallocType);
  MallocBlock* p = MallocBlock::Allocate(size, MallocBlock::kMallocType);

  // If realloc fails we are to leave the old block untouched and
  // return null
  if (p == nullptr)  return nullptr;

  size_t old_size = old->actual_data_size(ptr);

  memcpy(p->data_addr(), ptr, (old_size < size) ? old_size : size);
  tcmalloc::InvokeDeleteHook(ptr);
  tcmalloc::InvokeNewHook(p->data_addr(), size);
  DebugDeallocate(ptr, MallocBlock::kMallocType, 0);
  MALLOC_TRACE("realloc", p->actual_data_size(p->data_addr()), p->data_addr());
  return p->data_addr();
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new(size_t size) {
  void* ptr = debug_cpp_alloc(size, MallocBlock::kNewType, false);
  tcmalloc::InvokeNewHook(ptr, size);
  if (ptr == nullptr) {
    RAW_LOG(FATAL, "Unable to allocate %zu bytes: new failed.", size);
  }
  return ptr;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new_nothrow(size_t size, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  void* ptr = debug_cpp_alloc(size, MallocBlock::kNewType, true);
  tcmalloc::InvokeNewHook(ptr, size);
  return ptr;
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete(void* p) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kNewType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_sized(void* p, size_t size) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kNewType, size);
  force_frame();
}

// Some STL implementations explicitly invoke this.
// It is completely equivalent to a normal delete (delete never throws).
extern "C" PERFTOOLS_DLL_DECL void tc_delete_nothrow(void* p, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kNewType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray(size_t size) {
  void* ptr = debug_cpp_alloc(size, MallocBlock::kArrayNewType, false);
  tcmalloc::InvokeNewHook(ptr, size);
  if (ptr == nullptr) {
    RAW_LOG(FATAL, "Unable to allocate %zu bytes: new[] failed.", size);
  }
  return ptr;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray_nothrow(size_t size, const std::nothrow_t&)
    PERFTOOLS_NOTHROW {
  void* ptr = debug_cpp_alloc(size, MallocBlock::kArrayNewType, true);
  tcmalloc::InvokeNewHook(ptr, size);
  return ptr;
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray(void* p) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kArrayNewType, 0);
  force_frame();
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_sized(void* p, size_t size) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kArrayNewType, size);
  force_frame();
}

// Some STL implementations explicitly invoke this.
// It is completely equivalent to a normal delete (delete never throws).
extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_nothrow(void* p, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  tcmalloc::InvokeDeleteHook(p);
  DebugDeallocate(p, MallocBlock::kArrayNewType, 0);
  force_frame();
}

// This is mostly the same as do_memalign in tcmalloc.cc.
static void *do_debug_memalign(size_t alignment, size_t size, int type) {
  // Allocate >= size bytes aligned on "alignment" boundary
  // "alignment" is a power of two.
  void *p = 0;
  RAW_CHECK((alignment & (alignment-1)) == 0, "must be power of two");
  const size_t data_offset = MallocBlock::data_offset();
  // Allocate "alignment-1" extra bytes to ensure alignment is possible, and
  // a further data_offset bytes for an additional fake header.
  size_t extra_bytes = data_offset + alignment - 1;
  if (size + extra_bytes < size) return nullptr;         // Overflow
  p = DebugAllocate(size + extra_bytes, type);
  if (p != 0) {
    intptr_t orig_p = reinterpret_cast<intptr_t>(p);
    // Leave data_offset bytes for fake header, and round up to meet
    // alignment.
    p = reinterpret_cast<void *>(RoundUp(orig_p + data_offset, alignment));
    // Create a fake header block with an offset_ that points back to the
    // real header.  FromRawPointer uses this value.
    MallocBlock *fake_hdr = reinterpret_cast<MallocBlock *>(
                reinterpret_cast<char *>(p) - data_offset);
    // offset_ is distance between real and fake headers.
    // p is now end of fake header (beginning of client area),
    // and orig_p is the end of the real header, so offset_
    // is their difference.
    //
    // Note that other fields of fake_hdr are initialized with
    // kMagicUninitializedByte
    fake_hdr->set_offset(reinterpret_cast<intptr_t>(p) - orig_p);
  }
  return p;
}

struct memalign_retry_data {
  size_t align;
  size_t size;
  int type;
};

static void *retry_debug_memalign(void *arg) {
  memalign_retry_data *data = static_cast<memalign_retry_data *>(arg);
  return do_debug_memalign(data->align, data->size, data->type);
}

ALWAYS_INLINE
void* do_debug_memalign_or_debug_cpp_memalign(size_t align,
                                                     size_t size,
                                                     int type,
                                                     bool from_operator,
                                                     bool nothrow) {
  void* p = do_debug_memalign(align, size, type);
  if (p != nullptr) {
    return p;
  }

  struct memalign_retry_data data;
  data.align = align;
  data.size = size;
  data.type = type;
  return handle_oom(retry_debug_memalign, &data,
                    from_operator, nothrow);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_memalign(size_t align, size_t size) PERFTOOLS_NOTHROW {
  void *p = do_debug_memalign_or_debug_cpp_memalign(align, size, MallocBlock::kMallocType, false, true);
  tcmalloc::InvokeNewHook(p, size);
  return p;
}

// Implementation taken from tcmalloc/tcmalloc.cc
extern "C" PERFTOOLS_DLL_DECL int tc_posix_memalign(void** result_ptr, size_t align, size_t size)
    PERFTOOLS_NOTHROW {
  if (((align % sizeof(void*)) != 0) ||
      ((align & (align - 1)) != 0) ||
      (align == 0)) {
    return EINVAL;
  }

  void* result = do_debug_memalign_or_debug_cpp_memalign(align, size, MallocBlock::kMallocType, false, true);
  tcmalloc::InvokeNewHook(result, size);
  if (result == nullptr) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

extern "C" PERFTOOLS_DLL_DECL void* tc_valloc(size_t size) PERFTOOLS_NOTHROW {
  // Allocate >= size bytes starting on a page boundary
  void *p = do_debug_memalign_or_debug_cpp_memalign(getpagesize(), size, MallocBlock::kMallocType, false, true);
  tcmalloc::InvokeNewHook(p, size);
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_pvalloc(size_t size) PERFTOOLS_NOTHROW {
  // Round size up to a multiple of pages
  // then allocate memory on a page boundary
  int pagesize = getpagesize();
  size = RoundUp(size, pagesize);
  if (size == 0) {     // pvalloc(0) should allocate one page, according to
    size = pagesize;   // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  void *p = do_debug_memalign_or_debug_cpp_memalign(pagesize, size, MallocBlock::kMallocType, false, true);
  tcmalloc::InvokeNewHook(p, size);
  return p;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new_aligned(size_t size, std::align_val_t align) {
  void* result = do_debug_memalign_or_debug_cpp_memalign(static_cast<size_t>(align), size, MallocBlock::kNewType, true, false);
  tcmalloc::InvokeNewHook(result, size);
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new_aligned_nothrow(size_t size, std::align_val_t align, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  void* result = do_debug_memalign_or_debug_cpp_memalign(static_cast<size_t>(align), size, MallocBlock::kNewType, true, true);
  tcmalloc::InvokeNewHook(result, size);
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_aligned(void* p, std::align_val_t) PERFTOOLS_NOTHROW {
  tc_delete(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_sized_aligned(void* p, size_t size, std::align_val_t align) PERFTOOLS_NOTHROW {
  // Reproduce actual size calculation done by do_debug_memalign
  const size_t alignment = static_cast<size_t>(align);
  const size_t data_offset = MallocBlock::data_offset();
  const size_t extra_bytes = data_offset + alignment - 1;

  tc_delete_sized(p, size + extra_bytes);
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete_aligned_nothrow(void* p, std::align_val_t, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  tc_delete(p);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray_aligned(size_t size, std::align_val_t align) {
  void* result = do_debug_memalign_or_debug_cpp_memalign(static_cast<size_t>(align), size, MallocBlock::kArrayNewType, true, false);
  tcmalloc::InvokeNewHook(result, size);
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray_aligned_nothrow(size_t size, std::align_val_t align, const std::nothrow_t& nt) PERFTOOLS_NOTHROW {
  void* result = do_debug_memalign_or_debug_cpp_memalign(static_cast<size_t>(align), size, MallocBlock::kArrayNewType, true, true);
  tcmalloc::InvokeNewHook(result, size);
  return result;
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_aligned(void* p, std::align_val_t) PERFTOOLS_NOTHROW {
  tc_deletearray(p);
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_sized_aligned(void* p, size_t size, std::align_val_t align) PERFTOOLS_NOTHROW {
  // Reproduce actual size calculation done by do_debug_memalign
  const size_t alignment = static_cast<size_t>(align);
  const size_t data_offset = MallocBlock::data_offset();
  const size_t extra_bytes = data_offset + alignment - 1;

  tc_deletearray_sized(p, size + extra_bytes);
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray_aligned_nothrow(void* p, std::align_val_t, const std::nothrow_t&) PERFTOOLS_NOTHROW {
  tc_deletearray(p);
}

// malloc_stats just falls through to the base implementation.
extern "C" PERFTOOLS_DLL_DECL void tc_malloc_stats(void) PERFTOOLS_NOTHROW {
  do_malloc_stats();
}

extern "C" PERFTOOLS_DLL_DECL int tc_mallopt(int cmd, int value) PERFTOOLS_NOTHROW {
  return do_mallopt(cmd, value);
}

#ifdef HAVE_STRUCT_MALLINFO
extern "C" PERFTOOLS_DLL_DECL struct mallinfo tc_mallinfo(void) PERFTOOLS_NOTHROW {
  return do_mallinfo<struct mallinfo>();
}
#endif

#ifdef HAVE_STRUCT_MALLINFO2
extern "C" PERFTOOLS_DLL_DECL struct mallinfo2 tc_mallinfo2(void) PERFTOOLS_NOTHROW {
  return do_mallinfo<struct mallinfo2>();
}
#endif

extern "C" PERFTOOLS_DLL_DECL size_t tc_malloc_size(void* ptr) PERFTOOLS_NOTHROW {
  return MallocExtension::instance()->GetAllocatedSize(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_malloc_skip_new_handler(size_t size) PERFTOOLS_NOTHROW {
  void* result = DebugAllocate(size, MallocBlock::kMallocType);
  tcmalloc::InvokeNewHook(result, size);
  return result;
}
