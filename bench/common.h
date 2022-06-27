#pragma once

#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <unistd.h>

#include <climits>
#include <cstring>

#include "debug.h"

const char* filepath = []() -> const char* {
  const char* res = "test.txt";

  char* pmem_path = std::getenv("PMEM_PATH");
  if (pmem_path) {
    static char path[PATH_MAX];
    strcpy(path, pmem_path);
    strcat(path, "/test.txt");
    res = path;
  }

  fprintf(stderr, "================ filepath: %s ================ \n", res);
  return res;
}();

int get_num_iter(int default_val = 10000) noexcept {
  char* num_iter_str = std::getenv("BENCH_NUM_ITER");
  int num_iter = num_iter_str ? std::atoi(num_iter_str) : default_val;
  fprintf(stderr, "================ num_iter: %d =============== \n", num_iter);
  return num_iter;
}

void append_file(int fd, long num_bytes, int num_iter = 1) {
  auto buf = new char[num_bytes];
  std::fill(buf, buf + num_bytes, 'x');
  for (int i = 0; i < num_iter; ++i) {
    [[maybe_unused]] ssize_t res = write(fd, buf, num_bytes);
    assert(res == num_bytes);
  }
  fsync(fd);
  delete[] buf;
}

bool is_ulayfs_linked() { return ulayfs::debug::print_file != nullptr; }

void pin_core(int thread_index) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  // node0: 0-7,16-23
  // node1: 8-15,24-31
  if (thread_index <= 7) {
    CPU_SET(thread_index, &cpuset);
  } else if (thread_index <= 15) {
    CPU_SET(thread_index + 8, &cpuset);
  } else {
    fprintf(stderr, "thread_index %d is out of range on node 0.\n",
            thread_index);
  }
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
    fprintf(stderr, "sched_setaffinity failed for thread_index %d.\n",
            thread_index);
  }
}
