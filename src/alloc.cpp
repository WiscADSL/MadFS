#include "alloc.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "block.h"
#include "file.h"
#include "idx.h"

namespace ulayfs::dram {

LogicalBlockIdx Allocator::alloc(uint32_t num_blocks) {
  assert(num_blocks <= BITMAP_CAPACITY);

  // we first try to allocate from the free list
  auto it =
      std::lower_bound(free_list.begin(), free_list.end(),
                       std::pair<uint32_t, LogicalBlockIdx>(num_blocks, 0));
  if (it != free_list.end()) {
    auto idx = it->second;
    assert(idx != 0);
    TRACE("Allocator::alloc: allocating from free list: [%u, %u)", idx,
          idx + num_blocks);

    // exact match, remove from free list
    if (it->first == num_blocks) {
      free_list.erase(it);
      return idx;
    }

    // split a free list element
    if (it->first > num_blocks) {
      it->first -= num_blocks;
      it->second += num_blocks;
      // re-sort these elements
      std::sort(free_list.begin(), it + 1);
      return idx;
    }
  }

  // then we have to allocate from global bitmaps
  recent_bitmap_idx =
      Bitmap::alloc_batch(bitmap, NUM_BITMAP, recent_bitmap_idx);

  assert(recent_bitmap_idx >= 0);
  // push in decreasing order so pop will in increasing order
  LogicalBlockIdx allocated_idx = recent_bitmap_idx;
  if (num_blocks < BITMAP_CAPACITY) {
    free_list.emplace_back(BITMAP_CAPACITY - num_blocks,
                           allocated_idx + num_blocks);
    std::sort(free_list.begin(), free_list.end());
  }
  // this recent is not useful because we have taken all bits; move on
  recent_bitmap_idx++;
  TRACE("Allocator::alloc: allocating from bitmap: [%u, %u)", allocated_idx,
        allocated_idx + num_blocks);
  return allocated_idx;
}

void Allocator::free(LogicalBlockIdx block_idx, uint32_t num_blocks) {
  if (block_idx == 0) return;
  TRACE("Allocator::alloc: adding to free list: [%u, %u)", block_idx,
        num_blocks + block_idx);
  free_list.emplace_back(num_blocks, block_idx);
  std::sort(free_list.begin(), free_list.end());
}

void Allocator::free(const LogicalBlockIdx recycle_image[],
                     uint32_t image_size) {
  // try to group blocks
  // we don't try to merge the blocks with existing free list since the
  // searching is too expensive
  if (image_size == 0) return;
  uint32_t group_begin = 0;
  LogicalBlockIdx group_begin_lidx = 0;

  for (uint32_t curr = group_begin; curr < image_size; ++curr) {
    if (group_begin_lidx == 0) {  // new group not started yet
      if (recycle_image[curr] == 0) continue;
      // start a new group
      group_begin = curr;
      group_begin_lidx = recycle_image[curr];
    } else {
      // continue the group if it matches the expectation
      if (recycle_image[curr] == group_begin_lidx + (curr - group_begin))
        continue;
      TRACE("Allocator::free: adding to free list: [%u, %u)", group_begin_lidx,
            curr - group_begin + group_begin_lidx);
      free_list.emplace_back(curr - group_begin, group_begin_lidx);
      group_begin_lidx = recycle_image[curr];
      if (group_begin_lidx != 0) group_begin = curr;
    }
  }
  if (group_begin_lidx != 0) {
    TRACE("Allocator::free: adding to free list: [%u, %u)", group_begin_lidx,
          group_begin_lidx + image_size - group_begin);
    free_list.emplace_back(image_size - group_begin, group_begin_lidx);
  }
  std::sort(free_list.begin(), free_list.end());
}

pmem::LogEntry* Allocator::alloc_log_entry(
    bool pack_align, pmem::LogHeadEntry* prev_head_entry) {
  // if need 16-byte alignment, maybe skip one 8-byte slot
  if (pack_align) free_log_local_idx = ALIGN_UP(free_log_local_idx, 2);

  if (free_log_local_idx == NUM_LOG_ENTRY) {
    LogicalBlockIdx idx = alloc(1);
    log_blocks.push_back(idx);
    curr_log_block = &file->lidx_to_addr_rw(idx)->log_entry_block;
    free_log_local_idx = 0;
    if (prev_head_entry) prev_head_entry->next.next_block_idx = idx;
  } else {
    if (prev_head_entry)
      prev_head_entry->next.next_local_idx = free_log_local_idx;
  }

  assert(curr_log_block != nullptr);
  pmem::LogEntry* entry = curr_log_block->get(free_log_local_idx);
  memset(entry, 0, sizeof(pmem::LogEntry));  // zero-out at alloc

  free_log_local_idx++;
  return entry;
}

}  // namespace ulayfs::dram
