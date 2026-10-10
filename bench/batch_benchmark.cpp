#include "itoukv/store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
struct Temp {
  std::filesystem::path path;
  explicit Temp(std::size_t batch_size) {
    path = std::filesystem::temp_directory_path() / ("itoukv-batch-bench-" +
        std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(batch_size));
    if (!std::filesystem::create_directory(path)) throw itoukv::Error("temporary directory already exists");
  }
  ~Temp() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}

int main(int argc, char* argv[]) {
  std::size_t count = 1000;
  if (argc > 2) { std::cerr << "usage: itoukv_batch_bench [operations 1..100000]\n"; return 64; }
  if (argc == 2) {
    const std::string_view text(argv[1]);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || count == 0 || count > 100000) {
      std::cerr << "operations must be 1..100000\n"; return 64;
    }
  }
  try {
    std::cout << "{\"operations\":" << count << ",\"value_bytes\":128,\"sync_per_commit\":true,\"results\":[";
    bool first = true;
    for (std::size_t batch_size : {1U, 10U, 100U}) {
      Temp temp(batch_size);
      const std::string value(128, 'v');
      const auto commits = (count + batch_size - 1) / batch_size;
      double elapsed;
      std::uint64_t wal_bytes;
      {
        itoukv::Store store(temp.path);
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < count; offset += batch_size) {
          if (batch_size == 1) store.put("key-" + std::to_string(offset), value);
          else {
            itoukv::WriteBatch batch;
            for (std::size_t i = offset; i < std::min(count, offset + batch_size); ++i)
              batch.put("key-" + std::to_string(i), value);
            store.write(batch);
          }
        }
        elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto state = store.stats();
        if (state.sequence != commits || state.live_keys != count) throw itoukv::Error("batch accounting mismatch");
        wal_bytes = state.wal_bytes;
      }
      {
        itoukv::Store recovered(temp.path);
        if (recovered.stats().sequence != commits) throw itoukv::Error("batch recovery sequence mismatch");
        for (std::size_t i = 0; i < count; ++i)
          if (recovered.get("key-" + std::to_string(i)) != value) throw itoukv::Error("batch recovery value mismatch");
      }
      if (!first) std::cout << ',';
      first = false;
      std::cout << "{\"batch_size\":" << batch_size << ",\"commits\":" << commits
                << ",\"elapsed_ms\":" << elapsed << ",\"operations_per_second\":" << static_cast<double>(count) * 1000.0 / elapsed
                << ",\"wal_bytes\":" << wal_bytes << '}';
    }
    std::cout << "]}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
