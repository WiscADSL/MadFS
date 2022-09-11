#pragma once

#include <pthread.h>
#include <sys/xattr.h>

#include <ostream>

#include "const.h"
#include "logging.h"
#include "posix.h"
#include "utils.h"

namespace ulayfs::dram {

class alignas(SHM_PER_THREAD_SIZE) PerThreadData {
  enum State : uint8_t {
    UNINITIALIZED,
    INITIALIZING,
    INITIALIZED,
  };

  std::atomic<enum State> state;

 public:
  // mutex used to indicate the liveness of the thread
  // shall only be read when state == INITIALIZED
  pthread_mutex_t mutex;

  // the index within the shared memory region
  size_t index;

  // each thread will pin a tx block so that the garbage collector will not
  // reclaim this block and blocks after it
  LogicalBlockIdx tx_block_idx;

  /**
   * @return true if this PerThreadData is initialized and the thread is not
   * dead.
   */
  [[nodiscard]] bool is_valid() {
    if (state != INITIALIZED) return false;
    return is_thread_alive();
  }

  /**
   * Try to initialize the per-thread data. There should be only one thread
   * calling this function at a time.
   *
   * @param i the index of this per-thread data
   * @return true if initialization succeeded
   */
  bool try_init(size_t i) {
    State expected = UNINITIALIZED;
    if (!state.compare_exchange_strong(expected, INITIALIZING)) {
      // If the state is not UNINITIALIZED, then it must be INITIALIZED.
      // It cannot be INITIALIZING because there should be only one thread
      // calling this function.
      return false;
    }

    index = i;
    tx_block_idx = 0;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&mutex, &attr);
    state = State::INITIALIZED;
    return true;
  }

  /**
   * Destroy the per-thread data
   */
  void reset() {
    LOG_DEBUG("PerThreadData %ld reset by tid %d", index, tid);
    state = State::UNINITIALIZED;
    pthread_mutex_destroy(&mutex);
  }

 private:
  /**
   * Check the robust mutex to see if the thread is alive.
   *
   * This function can only be called when state == INITIALIZED, since trying to
   * lock an uninitialized mutex will cause undefined behavior.
   *
   * @return true if the thread is alive
   */
  bool is_thread_alive() {
    assert(state == State::INITIALIZED);
    int rc = pthread_mutex_trylock(&mutex);
    if (rc == 0) {
      // if we can lock the mutex, then the thread is dead
      pthread_mutex_unlock(&mutex);
      return false;
    } else if (rc == EBUSY) {
      // if the mutex is already locked, then the thread is alive
      return true;
    } else {
      PANIC("pthread_mutex_trylock failed: %s", strerror(rc));
    }
  }
};

static_assert(sizeof(PerThreadData) == SHM_PER_THREAD_SIZE);

class ShmMgr {
  pmem::MetaBlock* meta;
  int fd = -1;
  void* addr = nullptr;
  char path[SHM_PATH_LEN]{};

 public:
  /**
   * Open and memory map the shared memory. If the shared memory does not exist,
   * create it.
   *
   * @param file_fd the file descriptor of the file that uses this shared memory
   * @param stat the stat of the file that uses this shared memory
   */
  ShmMgr(int file_fd, const struct stat& stat, pmem::MetaBlock* meta)
      : meta(meta) {
    // get or set the path of the shared memory
    {
      ssize_t rc = fgetxattr(file_fd, SHM_XATTR_NAME, path, SHM_PATH_LEN);
      if (rc == -1 && errno == ENODATA) {  // no shm_path attribute, create one
        sprintf(path, "/dev/shm/ulayfs_%016lx_%013lx", stat.st_ino,
                (stat.st_ctim.tv_sec * 1000000000 + stat.st_ctim.tv_nsec) >> 3);
        rc = fsetxattr(file_fd, SHM_XATTR_NAME, path, SHM_PATH_LEN, 0);
        PANIC_IF(rc == -1, "failed to set shm_path attribute");
      } else if (rc == -1) {
        PANIC("failed to get shm_path attribute");
      }
    }

    // use posix::open instead of shm_open since shm_open calls open, which is
    // overloaded by ulayfs
    fd = posix::open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
      fd = create(path, stat.st_mode, stat.st_uid, stat.st_gid);
    }
    LOG_DEBUG("posix::open(%s) = %d", path, fd);

    addr = posix::mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, 0);
    if (addr == MAP_FAILED) {
      posix::close(fd);
      PANIC("mmap shared memory failed");
    }
  }

  ~ShmMgr() {
    if (fd >= 0) posix::close(fd);
    if (addr != nullptr) posix::munmap(addr, SHM_SIZE);
  }

  [[nodiscard]] void* get_bitmap_addr() const { return addr; }

  /**
   * Get the address of the per-thread data of the current thread.
   * Shall only be called by the garbage collector.
   *
   * @param idx the index of the per-thread data
   * @return the address of the per-thread data
   */
  [[nodiscard]] PerThreadData* get_per_thread_data(size_t idx) const {
    assert(idx < MAX_NUM_THREADS);
    char* starting_addr = static_cast<char*>(addr) + TOTAL_NUM_BITMAP_BYTES;
    return reinterpret_cast<PerThreadData*>(starting_addr) + idx;
  }

  /**
   * Allocate a new per-thread data for the current thread.
   * @return the address of the per-thread data
   */
  [[nodiscard]] PerThreadData* alloc_per_thread_data() const {
    meta->lock();
    for (size_t i = 0; i < MAX_NUM_THREADS; i++) {
      PerThreadData* per_thread_data = get_per_thread_data(i);
      bool success = per_thread_data->try_init(i);
      if (success) return per_thread_data;
    }
    meta->unlock();
    PANIC("No empty per-thread data");
  }

  /**
   * Remove the shared memory object associated.
   */
  void unlink() const { unlink_by_shm_path(path); }

  /**
   * Create a shared memory object.
   *
   * @param shm_path the path of the shared memory object
   * @param mode the mode of the shared memory object
   * @param uid the uid of the shared memory object
   * @param gid the gid of the shared memory object
   * @return the file descriptor of the shared memory object
   */
  static int create(const char* shm_path, mode_t mode, uid_t uid, gid_t gid) {
    // We create a temporary file first, and then use `linkat` to put the file
    // into the directory `/dev/shm`. This ensures the atomicity of the creating
    // the shared memory file and setting its permission.
    int shm_fd =
        posix::open("/dev/shm", O_TMPFILE | O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
    if (unlikely(shm_fd < 0)) {
      PANIC("create the temporary file failed");
    }

    // change permission and ownership of the new shared memory
    if (fchmod(shm_fd, mode) < 0) {
      posix::close(shm_fd);
      PANIC("fchmod on shared memory failed");
    }

    if (fchown(shm_fd, uid, gid) < 0) {
      posix::close(shm_fd);
      PANIC("fchown on shared memory failed");
    }

    if (posix::fallocate(shm_fd, 0, 0, static_cast<off_t>(SHM_SIZE)) < 0) {
      posix::close(shm_fd);
      PANIC("fallocate on shared memory failed");
    }

    // publish the created tmpfile.
    char tmpfile_path[PATH_MAX];
    sprintf(tmpfile_path, "/proc/self/fd/%d", shm_fd);
    int rc =
        linkat(AT_FDCWD, tmpfile_path, AT_FDCWD, shm_path, AT_SYMLINK_FOLLOW);
    if (rc < 0) {
      // Another process may have created a new shared memory before us. Retry
      // opening.
      posix::close(shm_fd);
      shm_fd = posix::open(shm_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                           S_IRUSR | S_IWUSR);
      if (shm_fd < 0) {
        PANIC("cannot open or create the shared memory object %s", shm_path);
      }
    }

    return shm_fd;
  }

  /**
   * Remove the shared memory object given its path.
   * @param shm_path the path of the shared memory object
   */
  static void unlink_by_shm_path(const char* shm_path) {
    int ret = posix::unlink(shm_path);
    LOG_TRACE("posix::unlink(%s) = %d", shm_path, ret);
    if (unlikely(ret < 0))
      LOG_WARN("Could not unlink shm file \"%s\": %m", shm_path);
  }

  /**
   * Remove the shared memory object given the path of the file that uses it.
   * @param filepath the path of the file that uses the shared memory object
   */
  static void unlink_by_file_path(const char* filepath) {
    char shm_path[SHM_PATH_LEN];
    if (getxattr(filepath, SHM_XATTR_NAME, shm_path, SHM_PATH_LEN) <= 0) return;
    unlink_by_shm_path(shm_path);
  }

  friend std::ostream& operator<<(std::ostream& os, const ShmMgr& mgr) {
    os << "ShmMgr:\n"
       << "\tfd = " << mgr.fd << "\n"
       << "\taddr = " << mgr.addr << "\n"
       << "\tpath = " << mgr.path << "\n";
    for (size_t i = 0; i < MAX_NUM_THREADS; ++i) {
      if (mgr.get_per_thread_data(i)->is_valid()) {
        os << "\tthread " << i << ": tail_tx_block_idx = "
           << mgr.get_per_thread_data(i)->tx_block_idx << "\n";
      }
    }
    return os;
  }
};

}  // namespace ulayfs::dram
