#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "common.h"
#include "const.h"
#include "posix.h"

char shm_path[ulayfs::CACHELINE_SIZE];

void create_file() {
  [[maybe_unused]] off_t res;
  [[maybe_unused]] ssize_t sz;

  int fd = open(FILEPATH, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  assert(fd >= 0);

  // create enough tx so that valid tx span beyond meta block
  int num_tx = ulayfs::NUM_INLINE_TX_ENTRY + ulayfs::NUM_TX_ENTRY + 1;
  for (int i = 0; i < num_tx; i++) {
    sz = write(fd, TEST_STR, TEST_STR_LEN);
    assert(sz == TEST_STR_LEN);
  }

  res = fsync(fd);
  assert(res == 0);

  int rc = ulayfs::posix::pread(fd, shm_path, ulayfs::CACHELINE_SIZE,
                                2 * ulayfs::CACHELINE_SIZE);
  assert(rc >= 0);

  // print_file(fd);

  res = close(fd);
  assert(res == 0);
}

void check_bitmap() {
  // reopen the file to build the dram bitmap
  int fd = open(FILEPATH, O_RDWR, S_IRUSR | S_IWUSR);
  assert(fd >= 0);

  int res = fsync(fd);
  assert(res == 0);

  // to ensure that dram and pmem bitmaps match
  //  print_file(fd);

  close(fd);
}

void cleanup() {
  [[maybe_unused]] int rc;

  rc = unlink(FILEPATH);
  assert(rc == 0);

  rc = unlink(shm_path);
  assert(rc == 0);
}

int main() {
  unlink(FILEPATH);
  create_file();
  check_bitmap();

  // remove the shared memory object so that it will recreate a new bitmap on
  // next opening.
  int res = unlink(shm_path);
  perror("unlink");
  assert(res == 0);
  check_bitmap();

  cleanup();
  return 0;
}
