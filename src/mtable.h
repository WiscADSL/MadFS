#pragma once

#include <linux/mman.h>

#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "config.h"
#include "layout.h"
#include "posix.h"
#include "utils.h"

namespace ulayfs::dram {

constexpr static uint32_t GROW_UNIT_IN_BLOCK_SHIFT =
    LayoutParams::grow_unit_shift - BLOCK_SHIFT;
constexpr static uint32_t GROW_UNIT_IN_BLOCK_MASK =
    (1 << GROW_UNIT_IN_BLOCK_SHIFT) - 1;
constexpr static uint32_t NUM_BLOCKS_PER_GROW =
    LayoutParams::grow_unit_size / BLOCK_SIZE;

// map index into address
// this is a more low-level data structure than Allocator
// it should maintain the virtualization of infinite large of file
// everytime it gets a LogicalBlockIdx:
// - if this block is already mapped; return addr
// - if this block is allocated from kernel filesystem, mmap and return
//   the addr
// - if this block is not even allocated from kernel filesystem, grow
//   it, map it, and return the address
class MemTable {
  pmem::MetaBlock* meta;
  int fd;

  // a copy of global num_blocks in MetaBlock to avoid shared memory access
  // may be out-of-date; must re-read global one if necessary
  uint32_t num_blocks_local_copy;

  std::unordered_map<pmem::LogicalBlockIdx, pmem::Block*> table;

 private:
  // called by other public functions with lock held
  void grow_no_lock(pmem::LogicalBlockIdx idx) {
    // we need to revalidate under after acquiring lock
    if (idx < meta->get_num_blocks()) return;
    uint32_t new_num_blocks = ((idx >> LayoutParams::grow_unit_shift) + 1)
                              << LayoutParams::grow_unit_shift;
    int ret =
        posix::ftruncate(fd, static_cast<long>(new_num_blocks) << BLOCK_SHIFT);
    if (ret) throw std::runtime_error("Fail to ftruncate!");
    meta->set_num_blocks_no_lock(new_num_blocks);
  }

 public:
  MemTable() : fd(-1), num_blocks_local_copy(0), table(){};

  pmem::MetaBlock* init(int fd, off_t file_size) {
    this->fd = fd;
    // file size should be block-aligned
    if (!IS_ALIGNED(file_size, BLOCK_SIZE))
      throw std::runtime_error("Invalid layout: non-block-aligned file size!");

    // grow to multiple of grow_unit_size if the file is empty or the file size
    // is not grow_unit aligned
    if (file_size == 0 ||
        !IS_ALIGNED(file_size, LayoutParams::grow_unit_size)) {
      file_size = file_size == 0
                      ? LayoutParams::prealloc_size
                      : ((file_size >> LayoutParams::grow_unit_shift) + 1)
                            << LayoutParams::grow_unit_shift;
      int ret = posix::ftruncate(fd, file_size);
      if (ret) throw std::runtime_error("Fail to ftruncate!");
    }

    int mmap_flags = MAP_SHARED;
    if constexpr (BuildOptions::use_hugepage) {
      mmap_flags |= MAP_HUGETLB | MAP_HUGE_2MB;
    }
    void* addr = posix::mmap(nullptr, file_size, PROT_READ | PROT_WRITE,
                             mmap_flags, fd, 0);
    if (addr == (void*)-1) throw std::runtime_error("Fail to mmap!");
    auto blocks = static_cast<pmem::Block*>(addr);
    this->meta = &blocks->meta_block;

    // initialize the mapping
    uint32_t num_blocks = file_size >> BLOCK_SHIFT;
    for (pmem::LogicalBlockIdx idx = 0; idx < num_blocks;
         idx += NUM_BLOCKS_PER_GROW)
      table.emplace(idx, blocks + idx);

    this->meta->set_num_blocks_no_lock(num_blocks);
    this->num_blocks_local_copy = num_blocks;

    return this->meta;
  }

  // ask more blocks for the kernel filesystem, so that idx is valid
  void validate(pmem::LogicalBlockIdx idx) {
    // fast path: if smaller than local copy; return
    if (idx < num_blocks_local_copy) return;

    // medium path: update local copy and retry
    num_blocks_local_copy = meta->get_num_blocks();
    if (idx < num_blocks_local_copy) return;

    // slow path: acquire lock to verify and grow if necessary
    meta->lock();
    grow_no_lock(idx);
    meta->unlock();
  }

  // the idx might pass Allocator's grow() to ensure there is a backing kernel
  // filesystem block
  // get_addr will then check if it has been mapped into the address space; if
  // not, it does mapping first
  pmem::Block* get_addr(pmem::LogicalBlockIdx idx) {
    pmem::LogicalBlockIdx hugepage_idx = idx & ~GROW_UNIT_IN_BLOCK_MASK;
    auto offset = ((idx & GROW_UNIT_IN_BLOCK_MASK) << BLOCK_SHIFT);
    auto it = table.find(hugepage_idx);
    if (it != table.end()) return it->second + offset;

    // validate if this idx has real blocks allocated; do allocation if not
    validate(idx);

    off_t hugepage_size = static_cast<off_t>(hugepage_idx) << BLOCK_SHIFT;
    int mmap_flags = MAP_SHARED;
    if constexpr (BuildOptions::use_hugepage) {
      mmap_flags |= MAP_HUGETLB | MAP_HUGE_2MB;
    }
    void* addr =
        posix::mmap(nullptr, LayoutParams::prealloc_size,
                    PROT_READ | PROT_WRITE, mmap_flags, fd, hugepage_size);
    if (addr == (void*)-1) throw std::runtime_error("Fail to mmap!");
    auto hugepage_blocks = static_cast<pmem::Block*>(addr);
    table.emplace(hugepage_idx, hugepage_blocks);
    return hugepage_blocks + offset;
  }

  friend std::ostream& operator<<(std::ostream& out, const MemTable& m) {
    out << "MemTable:\n";
    out << "\tnum_blocks_local_copy: " << m.num_blocks_local_copy << "\n";
    out << "\ttable: \n";
    for (const auto& [blk_idx, mem_addr] : m.table) {
      out << "\t\tblk_idx: " << blk_idx << ", mem_addr: " << mem_addr;
    }
    return out;
  }
};

};  // namespace ulayfs::dram
