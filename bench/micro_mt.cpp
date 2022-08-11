#include <sys/stat.h>

#include "common.h"
#include "zipf.h"

#ifdef NDEBUG
constexpr static bool debug = false;
#else
constexpr static bool debug = true;
#endif

constexpr int BLOCK_SIZE = 4096;
constexpr int MAX_NUM_THREAD = 16;
constexpr double ZIPF_THETA = 0.9;

const char* filepath = get_filepath();
int num_iter = get_num_iter();
int file_size = get_file_size();

int fd;

enum class Mode {
  UNIF,
  APPEND,
  ZIPF,
};

template <Mode mode, int READ_PERCENT = -1>
void bench(benchmark::State& state) {
  const auto num_bytes = state.range(0);

  pin_core(state.thread_index);

  [[maybe_unused]] char dst_buf[BLOCK_SIZE * MAX_NUM_THREAD];
  [[maybe_unused]] char src_buf[BLOCK_SIZE * MAX_NUM_THREAD];
  std::fill(src_buf, src_buf + BLOCK_SIZE * MAX_NUM_THREAD, 'x');

  if (state.thread_index == 0) {
    if constexpr (mode == Mode::APPEND) {
      unlink(filepath);
    }

    fd = open(filepath, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) state.SkipWithError("open failed");

    // preallocate file
    if constexpr (mode != Mode::APPEND) {
      // check file size
      struct stat st;  // NOLINT(cppcoreguidelines-pro-type-member-init)
      fstat(fd, &st);
      if (st.st_size != file_size) {
        prefill_file(fd, file_size);
      }
    }
  }

  if (is_ulayfs_linked()) ulayfs::debug::clear_count();

  // run benchmark
  if constexpr (mode == Mode::APPEND) {
    for (auto _ : state) {
      [[maybe_unused]] ssize_t res = write(fd, src_buf, num_bytes);
      assert(res == num_bytes);
      fsync(fd);
    }
    if constexpr (debug) {
      if (state.thread_index == 0) {
        [[maybe_unused]] ssize_t res = lseek(fd, 0, SEEK_SET);
        for (int i = 0; i < num_iter; i++) {
          res = read(fd, dst_buf, num_bytes);
          if (res != num_bytes) {
            fprintf(stderr, "expected = %ld, actual = %ld\n",
                    num_iter * num_bytes, i * num_bytes);
            break;
          }
        }
      }
    }
  } else if constexpr (mode == Mode::UNIF) {
    bool is_read[num_iter];
    std::generate(is_read, is_read + num_iter,
                  [&]() { return rand() % 100 < READ_PERCENT; });

    int rand_off[num_iter];
    std::generate(rand_off, rand_off + num_iter, [&]() {
      return rand() % (file_size / num_bytes) * num_bytes;
    });

    int i = 0;
    for (auto _ : state) {
      if (is_read[i]) {
        [[maybe_unused]] ssize_t res =
            pread(fd, dst_buf, num_bytes, rand_off[i]);
        assert(res == num_bytes);
        assert(memcmp(dst_buf, src_buf, num_bytes) == 0);
      } else {
        [[maybe_unused]] ssize_t res =
            pwrite(fd, src_buf, num_bytes, rand_off[i]);
        assert(res == num_bytes);
        fsync(fd);
      }
      i++;
    }
  } else if constexpr (mode == Mode::ZIPF) {
    std::default_random_engine generator;
    zipfian_int_distribution<int> zipf(1, file_size / BLOCK_SIZE, ZIPF_THETA);

    off_t offset[num_iter];
    std::generate(offset, offset + num_iter, [&]() { return zipf(generator); });
    int i = 0;
    for (auto _ : state) {
      [[maybe_unused]] ssize_t res =
          pwrite(fd, src_buf, num_bytes, offset[i++] * BLOCK_SIZE);
      assert(res == num_bytes);
      fsync(fd);
    }
  }

  // tear down
  if (state.thread_index == 0) {
    close(fd);
  }

  // report result
  auto items_processed = static_cast<int64_t>(state.iterations());
  auto bytes_processed = items_processed * num_bytes;
  state.SetBytesProcessed(bytes_processed);
  state.SetItemsProcessed(items_processed);

  if (is_ulayfs_linked()) {
    double start_cnt =
        ulayfs::debug::get_count(ulayfs::debug::SINGLE_BLOCK_TX_START) +
        ulayfs::debug::get_count(ulayfs::debug::ALIGNED_TX_START);
    double copy_cnt =
        ulayfs::debug::get_count(ulayfs::debug::SINGLE_BLOCK_TX_COPY);
    double commit_cnt =
        ulayfs::debug::get_count(ulayfs::debug::SINGLE_BLOCK_TX_COMMIT) +
        ulayfs::debug::get_count(ulayfs::debug::ALIGNED_TX_COMMIT);

    if (start_cnt != 0 && commit_cnt != 0) {
      state.counters["tx_copy"] = copy_cnt / start_cnt / state.threads;
      state.counters["tx_commit"] = commit_cnt / start_cnt / state.threads;
    }

    ulayfs::debug::clear_count();
  }
}

template <class F>
auto register_bm(const char* name, F f, int num_bytes = BLOCK_SIZE) {
  return benchmark::RegisterBenchmark(name, f)
      ->Args({num_bytes})
      ->DenseThreadRange(1, MAX_NUM_THREAD)
      ->Iterations(num_iter)
      ->UseRealTime();
}

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  unlink(filepath);

  register_bm("unif_0R", bench<Mode::UNIF, 0>);
  register_bm("unif_50R", bench<Mode::UNIF, 50>);
  register_bm("unif_95R", bench<Mode::UNIF, 95>);
  register_bm("unif_100R", bench<Mode::UNIF, 100>);

  register_bm("zipf_4k", bench<Mode::ZIPF>, 4096);
  register_bm("zipf_2k", bench<Mode::ZIPF>, 2048);

  register_bm("append_512", bench<Mode::APPEND>, 512);
  register_bm("append_4k", bench<Mode::APPEND>);

  benchmark::RunSpecifiedBenchmarks();
  return 0;
}
