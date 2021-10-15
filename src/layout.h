#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "futex.h"

namespace ulayfs::pmem {

using BlockIdx = uint32_t;

// All member functions are thread-safe and require no locks
class Bitmap {
  uint64_t bitmap;

 public:
  constexpr static uint64_t BITMAP_ALL_USED = 0xffffffffffffffff;

  // return the index of the bit; -1 if fail
  int alloc_one() {
  retry:
    uint64_t b = __atomic_load_n(&bitmap, __ATOMIC_ACQUIRE);
    if (b == BITMAP_ALL_USED) return -1;
    uint64_t allocated = (~b) & (b + 1);  // which bit is allocated
    // if bitmap is exactly the same as we saw previously, set it allocated
    if (!__atomic_compare_exchange_n(&bitmap, &b, b & allocated, true,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      goto retry;
    return std::countr_zero(b);
  }

  // allocate all blocks in this bit; return 0 if succeeds, -1 otherwise
  int alloc_all() {
    uint64_t expected = 0;
    if (__atomic_load_n(&bitmap, __ATOMIC_ACQUIRE) != 0) return -1;
    if (!__atomic_compare_exchange_n(&bitmap, &expected, BITMAP_ALL_USED, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return -1;
    return 0;
  }

  void set_allocated(uint32_t idx) { bitmap |= (1 << idx); }
};

class TxEntry {
 public:
  uint64_t entry;
};

class TxBeginEntry : public TxEntry {};

class TxCommitEntry : public TxEntry {};

enum LogOp : uint32_t {
  LOG_OVERWRITE,
};

class LogEntry {
  enum LogOp op;
  BlockIdx file_offset;
  BlockIdx block_offset;
  uint32_t size;
};

constexpr static uint32_t BLOCK_SIZE = 4096;
constexpr static uint32_t CACHELINE_SIZE = 64;
constexpr static uint32_t NUM_BITMAP = BLOCK_SIZE / sizeof(Bitmap);
constexpr static uint32_t NUM_TX_ENTRY =
    (BLOCK_SIZE - 2 * sizeof(BlockIdx)) / sizeof(TxEntry);
constexpr static uint32_t NUM_LOG_ENTRY = BLOCK_SIZE / sizeof(LogEntry);
constexpr static uint32_t NUM_CL_BITMAP_IN_META = 2;
constexpr static uint32_t NUM_CL_TX_ENTRY_IN_META =
    ((BLOCK_SIZE / CACHELINE_SIZE) - 2) - NUM_CL_BITMAP_IN_META;
constexpr static uint32_t NUM_INLINE_BITMAP =
    NUM_CL_BITMAP_IN_META * (CACHELINE_SIZE / sizeof(Bitmap));
constexpr static uint32_t NUM_INLINE_TX_ENTRY =
    NUM_CL_TX_ENTRY_IN_META * (CACHELINE_SIZE / sizeof(TxEntry));

/*
 * Idx: 0          1          2
 * +----------+----------+----------+----------+----------+----------+----------
 * |   Meta   | Bitmap 1 | Bitmap 2 |   ...    |   ...    | Data/Log |   ...
 * +----------+----------+----------+----------+----------+----------+----------
 * Note: The first few blocks following the meta block is always bitmap blocks
 */

class MetaBlock {
  // contents in the first cache line
  union {
    struct {
      // file signature
      char signature[16];

      // file size in bytes (logical size to users)
      uint64_t file_size;

      // total number of blocks actually in this file (including unused ones)
      uint32_t num_blocks;

      // number of blocks following the meta block that are bitmap blocks
      uint32_t num_bitmap_blocks;

      // if inline_tx_entries is used up, this points to the next log block
      BlockIdx log_head;

      // hint to find log tail; not necessarily up-to-date
      BlockIdx log_tail;
    };  // all fields modificaton above requires futex acquired

    // padding avoid cache line contention
    char padding[CACHELINE_SIZE];
  };

  union {
    // address for futex to lock, 4 bytes in size
    Futex meta_lock;

    // set futex to another cacheline to avoid futex's contention affect reading
    // the metadata above
    char padding[CACHELINE_SIZE];
  };

  // for the rest of 62 cache lines:
  // 2 cache lines for bitmaps (~1024 blocks = 4M)
  Bitmap inline_bitmaps[NUM_INLINE_BITMAP];

  // 60 cache lines for tx log (~480 txs)
  TxEntry inline_tx_entries[NUM_INLINE_TX_ENTRY];

 public:
  // only called if a new file is created
  void init() {
    // the first block is always used (by MetaBlock itself)
    inline_bitmaps[0].set_allocated(0);
    strcpy(signature, "ULAYFS");
  }

  // allocate one block; return the index of allocated block
  // accept a hint for which bit to start searching
  // usually hint can just be the last idx return by this function
  int inline_alloc_one(int hint = 0) {
    int ret;
    for (int idx = (hint >> 6); idx < NUM_INLINE_BITMAP; ++idx) {
      ret = inline_bitmaps[idx].alloc_one();
      if (ret < 0) continue;
      return (idx << 6) + ret;
    }
    return -1;
  }

  // 64 blocks are considered as one batch; return the index of the first block
  int inline_alloc_batch(int hint = 0) {
    int ret = 0;
    for (int idx = (hint >> 6); idx < NUM_INLINE_TX_ENTRY; ++idx) {
      ret = inline_bitmaps[idx].alloc_all();
      if (ret < 0) continue;
      return (idx << 6);
    }
    return -1;
  }
};

class BitmapBlock {
  Bitmap bitmaps[NUM_BITMAP];

 public:
  // allocate one block; return the index of allocated block
  // accept a hint for which bit to start searching
  // usually hint can just be the last idx return by this function
  int alloc_one(int hint = 0) {
    int ret;
    for (int idx = (hint >> 6); idx < NUM_BITMAP; ++idx) {
      ret = bitmaps[idx].alloc_one();
      if (ret < 0) continue;
      return (idx << 6) + ret;
    }
    return -1;
  }

  // 64 blocks are considered as one batch; return the index of the first block
  int alloc_batch(int hint = 0) {
    int ret = 0;
    for (int idx = (hint >> 6); idx < NUM_BITMAP; ++idx) {
      ret = bitmaps[idx].alloc_all();
      if (ret < 0) continue;
      return (idx << 6);
    }
    return -1;
  }

  // map `in_bitmap_idx` from alloc_one/all to the actual BlockIdx
  // bitmap_block_idx = 0 if from inline_bitmap_block in MetaBlock
  static BlockIdx get_block_idx(BlockIdx bitmap_block_idx, int in_bitmap_idx) {
    if (bitmap_block_idx == 0) return in_bitmap_idx;
    return ((NUM_INLINE_BITMAP + (bitmap_block_idx - 1) * NUM_BITMAP) << 6) +
           in_bitmap_idx;
  }
};

class TxLogBlock {
  BlockIdx prev;
  BlockIdx next;
  TxEntry tx_entries[NUM_TX_ENTRY];

 public:
  int try_commit(TxCommitEntry commit_entry, uint32_t hint_tail = 0) {
    for (auto idx = hint_tail; idx < NUM_LOG_ENTRY; ++idx) {
      uint64_t expected = 0;
      if (__atomic_load_n(&tx_entries[idx].entry, __ATOMIC_ACQUIRE)) continue;
      if (__atomic_compare_exchange_n(&tx_entries[idx].entry, &expected,
                                      commit_entry.entry, false,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return idx;
    }
    return -1;
  }
};

class LogEntryBlock {
  LogEntry log_entries[NUM_LOG_ENTRY];
};

class DataBlock {
  char data[BLOCK_SIZE];
};

union Block {
  MetaBlock meta_block;
  BitmapBlock bitmap_block;
  TxLogBlock tx_log_block;
  LogEntryBlock log_entry_block;
  DataBlock data_block;
  char padding[BLOCK_SIZE];
};

static_assert(sizeof(Bitmap) == 8, "Bitmap must of 64 bits");
static_assert(sizeof(TxEntry) == 8, "TxEntry must be 64 bits");
static_assert(sizeof(LogEntry) == 16, "LogEntry must of size 16 bytes");
static_assert(sizeof(MetaBlock) == BLOCK_SIZE,
              "MetaBlock must be of size BLOCK_SIZE");
static_assert(sizeof(BitmapBlock) == BLOCK_SIZE,
              "BitmapBlock must be of size BLOCK_SIZE");
static_assert(sizeof(TxLogBlock) == BLOCK_SIZE,
              "TxLogBlock must be of size BLOCK_SIZE");
static_assert(sizeof(LogEntryBlock) == BLOCK_SIZE,
              "LogEntryBlock must be of size BLOCK_SIZE");
static_assert(sizeof(DataBlock) == BLOCK_SIZE,
              "DataBlock must be of size BLOCK_SIZE");
static_assert(sizeof(Block) == BLOCK_SIZE, "Block must be of size BLOCK_SIZE");

};  // namespace ulayfs::pmem
