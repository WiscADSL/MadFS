#include "mgr.h"

#include <cmath>
#include <cstddef>
#include <vector>

#include "alloc/alloc.h"
#include "const.h"
#include "entry.h"
#include "file.h"
#include "idx.h"
#include "tx/read.h"
#include "tx/tx_cursor.h"
#include "tx/write_aligned.h"
#include "tx/write_unaligned.h"

namespace ulayfs::dram {

ssize_t TxMgr::do_pread(char* buf, size_t count, size_t offset) {
  TimerGuard<Event::READ_TX> timer_guard;
  timer.start<Event::READ_TX_CTOR>();
  return ReadTx(file, this, buf, count, offset).exec();
}

ssize_t TxMgr::do_read(char* buf, size_t count) {
  FileState state;
  uint64_t ticket;
  uint64_t offset;
  file->update_with_offset(&state, count,
                           /*stop_at_boundary*/ true, ticket, offset);

  return Tx::exec_and_release_offset<ReadTx>(file, this, buf, count, offset,
                                             state, ticket);
}

ssize_t TxMgr::do_pwrite(const char* buf, size_t count, size_t offset) {
  // special case that we have everything aligned, no OCC
  if (count % BLOCK_SIZE == 0 && offset % BLOCK_SIZE == 0) {
    TimerGuard<Event::ALIGNED_TX> timer_guard;
    timer.start<Event::ALIGNED_TX_CTOR>();
    return AlignedTx(file, this, buf, count, offset).exec();
  }

  // another special case where range is within a single block
  if ((BLOCK_SIZE_TO_IDX(offset)) == BLOCK_SIZE_TO_IDX(offset + count - 1)) {
    TimerGuard<Event::SINGLE_BLOCK_TX> timer_guard;
    return SingleBlockTx(file, this, buf, count, offset).exec();
  }

  // unaligned multi-block write
  {
    TimerGuard<Event::MULTI_BLOCK_TX> timer_guard;
    return MultiBlockTx(file, this, buf, count, offset).exec();
  }
}

ssize_t TxMgr::do_write(const char* buf, size_t count) {
  FileState state;
  uint64_t ticket;
  uint64_t offset;
  file->update_with_offset(&state, count,
                           /*stop_at_boundary*/ false, ticket, offset);

  // special case that we have everything aligned, no OCC
  if (count % BLOCK_SIZE == 0 && offset % BLOCK_SIZE == 0) {
    TimerGuard<Event::ALIGNED_TX> timer_guard;
    return Tx::exec_and_release_offset<AlignedTx>(file, this, buf, count,
                                                  offset, state, ticket);
  }

  // another special case where range is within a single block
  if (BLOCK_SIZE_TO_IDX(offset) == BLOCK_SIZE_TO_IDX(offset + count - 1)) {
    TimerGuard<Event::SINGLE_BLOCK_TX> timer_guard;
    return Tx::exec_and_release_offset<SingleBlockTx>(file, this, buf, count,
                                                      offset, state, ticket);
  }

  // unaligned multi-block write
  {
    TimerGuard<Event::MULTI_BLOCK_TX> timer_guard;
    return Tx::exec_and_release_offset<MultiBlockTx>(file, this, buf, count,
                                                     offset, state, ticket);
  }
}

LogCursor TxMgr::append_log_entry(
    Allocator* allocator, pmem::LogEntry::Op op, uint16_t leftover_bytes,
    uint32_t num_blocks, VirtualBlockIdx begin_vidx,
    const std::vector<LogicalBlockIdx>& begin_lidxs) const {
  const LogCursor head = allocator->log_entry.alloc(num_blocks);
  LogCursor log_cursor = head;

  // i to iterate through begin_lidxs across entries
  // j to iterate within each entry
  uint32_t i, j;
  i = 0;
  while (true) {
    log_cursor->op = op;
    log_cursor->begin_vidx = begin_vidx;
    for (j = 0; j < log_cursor->get_lidxs_len(); ++j)
      log_cursor->begin_lidxs[j] = begin_lidxs[i + j];
    if (log_cursor->has_next) {
      log_cursor->leftover_bytes = 0;
      log_cursor->persist();
      i += j;
      begin_vidx += (j << BITMAP_ENTRY_BLOCKS_CAPACITY_SHIFT);
      log_cursor.advance(mem_table);
    } else {  // last entry
      log_cursor->leftover_bytes = leftover_bytes;
      log_cursor->persist();
      break;
    }
  }
  return head;
}

std::ostream& operator<<(std::ostream& out, const TxMgr& tx_mgr) {
  out << tx_mgr.offset_mgr;
  out << "Transactions: \n";

  TxCursor cursor(tx_mgr.file->meta);
  int count = 0;

  while (true) {
    auto tx_entry = cursor.get_entry();
    if (!tx_entry.is_valid()) break;
    if (tx_entry.is_dummy()) goto next;

    count++;
    if (count > 100) {
      if (count % static_cast<int>(exp10(floor(log10(count)) - 1)) != 0)
        goto next;
    }

    out << "\t" << count << ": " << cursor.idx << " -> " << tx_entry << "\n";

    // print log entries if the tx is not inlined
    if (!tx_entry.is_inline()) {
      LogCursor log_cursor(tx_entry.indirect_entry, tx_mgr.mem_table);
      do {
        out << "\t\t" << *log_cursor << "\n";
      } while (log_cursor.advance(tx_mgr.mem_table));
    }

  next:
    if (bool success = cursor.advance(tx_mgr.mem_table); !success) break;
  }

  out << "\ttotal = " << count++ << "\n";

  return out;
}

}  // namespace ulayfs::dram
