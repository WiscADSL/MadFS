#include <iostream>

#include "convert.h"
#include "file.h"
#include "lib/lib.h"
#include "posix.h"

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <file>" << std::endl;
    return 1;
  }

  const char *filename = argv[1];

  int fd = open(filename, O_RDWR);
  if (fd < 0) {
    std::cerr << "Failed to open " << filename << ": " << strerror(errno)
              << std::endl;
    return 1;
  }

  auto file = ulayfs::get_file(fd);

  if (!file) {
    std::cerr << filename << " is not a uLayFS file. \n";
    return 0;
  }

  fd = ulayfs::utility::Converter::convert_from(file.get());
  // now fd is just a normal file
  ulayfs::posix::close(fd);

  return 0;
}
