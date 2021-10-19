#include "lib.h"

#include <cstdarg>
#include <cstdio>

#include "config.h"
#include "layout.h"
#include "posix.h"

namespace ulayfs {
extern "C" {
int open(const char *pathname, int flags, ...) {
  mode_t mode = 0;

  if (__OPEN_NEEDS_MODE(flags)) {
    va_list arg;
    va_start(arg, flags);
    mode = va_arg(arg, mode_t);
    va_end(arg);
  }

  auto file = new dram::File();
  int fd = file->open(pathname, flags, mode);
  files[fd] = file;
  return fd;
}

ssize_t write(int fd, const void *buf, size_t count) {
  if constexpr (BuildOptions::debug) {
    printf("write:count:%lu\n", count);
  }
  return posix::write(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {
  if constexpr (BuildOptions::debug) {
    printf("read:count:%lu\n", count);
  }
  return posix::read(fd, buf, count);
}
}  // extern "C"
}  // namespace ulayfs
