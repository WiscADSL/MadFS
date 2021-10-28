#pragma once

#include <iostream>
#include <stdexcept>

#include "alloc.h"
#include "block.h"
#include "btable.h"
#include "config.h"
#include "layout.h"
#include "mtable.h"
#include "posix.h"
#include "tx.h"
#include "utils.h"

// data structure under this namespace must be in volatile memory (DRAM)
namespace ulayfs::dram {

class File {
  int fd = -1;
  int open_flags;

  bool is_ulayfs_file;

  pmem::MetaBlock* meta{nullptr};
  MemTable mem_table;
  BlkTable blk_table;
  Allocator allocator;
  TxMgr tx_mgr;

 private:
  /**
   * Write data to the shadow page starting from start_logical_idx
   *
   * @param buf the buffer given by the user
   * @param count number of bytes in the buffer
   * @param local_offset the start offset within the first block
   */
  void write_data(const void* buf, size_t count, uint64_t local_offset,
                  VirtualBlockIdx& start_virtual_idx,
                  LogicalBlockIdx& start_logical_idx) {
    // the address of the start of the new blocks
    char* dst = mem_table.get_addr(start_logical_idx)->data;

    // if the offset is not block-aligned, copy the remaining bytes at the
    // beginning to the shadow page
    if (local_offset) {
      auto src_idx = blk_table.get(start_virtual_idx);
      char* src = mem_table.get_addr(src_idx)->data;
      memcpy(dst, src, local_offset);
    }

    // write the actual buffer
    memcpy(dst + local_offset, buf, count);

    // persist the changes
    pmem::persist_fenced(dst, count + local_offset);
  }

  /**
   * @param virtual_block_idx the virtual block index for a data block
   * @return the char pointer pointing to the memory location of the data block
   */
  char* get_data_block_ptr(VirtualBlockIdx virtual_block_idx) {
    auto logical_block_idx = blk_table.get(virtual_block_idx);
    assert(logical_block_idx != 0);
    auto block = mem_table.get_addr(logical_block_idx);
    return block->data;
  }

 public:
  File() = default;

  // test if File is in a valid state
  explicit operator bool() const { return is_ulayfs_file && fd >= 0; }
  bool operator!() const { return !bool(this); }

  pmem::MetaBlock* get_meta() { return meta; }

  int get_fd() const { return fd; }

  /**
   * We use File::open to construct a File object instead of the standard
   * constructor since open may fail, and we want to report the return value
   * back to the caller
   */
  int open(const char* pathname, int flags, mode_t mode);

  /**
   * overwrite the byte range [offset, offset + count) with the content in buf
   */
  ssize_t overwrite(const void* buf, size_t count, size_t offset) {
    VirtualBlockIdx start_virtual_idx = ALIGN_DOWN(offset, BLOCK_SIZE);

    uint64_t local_offset = offset - start_virtual_idx * BLOCK_SIZE;
    uint32_t num_blocks =
        ALIGN_UP(count + local_offset, BLOCK_SIZE) >> BLOCK_SHIFT;

    auto tx_begin_idx = tx_mgr.begin_tx(start_virtual_idx, num_blocks);

    // TODO: handle the case where num_blocks > 64

    LogicalBlockIdx start_logical_idx = allocator.alloc(num_blocks);
    write_data(buf, count, local_offset, start_virtual_idx, start_logical_idx);

    uint16_t last_remaining = num_blocks * BLOCK_SIZE - count - local_offset;
    auto log_entry_idx = tx_mgr.write_log_entry(
        start_virtual_idx, start_logical_idx, num_blocks, last_remaining);

    tx_mgr.commit_tx(tx_begin_idx, log_entry_idx);

    blk_table.update();

    return static_cast<ssize_t>(count);
  }

  /**
   * read_entry the byte range [offset, offset + count) to buf
   */
  ssize_t pread(void* buf, size_t count, off_t offset) {
    VirtualBlockIdx start_virtual_idx = ALIGN_DOWN(offset, BLOCK_SIZE);

    uint64_t local_offset = offset - start_virtual_idx * BLOCK_SIZE;
    uint32_t num_blocks =
        ALIGN_UP(count + local_offset, BLOCK_SIZE) >> BLOCK_SHIFT;
    uint16_t last_remaining = num_blocks * BLOCK_SIZE - count - local_offset;

    char* dst = static_cast<char*>(buf);
    for (int i = 0; i < num_blocks; ++i) {
      size_t num_bytes = BLOCK_SIZE;
      if (i == 0) num_bytes -= local_offset;
      if (i == num_blocks - 1) num_bytes -= last_remaining;

      char* ptr = get_data_block_ptr(start_virtual_idx + i);
      char* src = i == 0 ? ptr + local_offset : ptr;

      memcpy(dst, src, num_bytes);
      dst += num_bytes;
    }

    return static_cast<ssize_t>(count);
  }

  friend std::ostream& operator<<(std::ostream& out, const File& f) {
    out << "File: fd = " << f.fd << "\n";
    out << *f.meta;
    out << f.mem_table;
    out << f.tx_mgr;
    out << f.blk_table;
    out << "\n";

    return out;
  }
};

}  // namespace ulayfs::dram
