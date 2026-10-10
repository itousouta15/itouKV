#include "itoukv/store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>

int main(int argc, char* argv[]) {
  std::size_t count = 500;
  if (argc > 2) { std::cerr << "usage: itoukv_bench [operations]\n"; return 64; }
  if (argc == 2) {
    const std::string_view text(argv[1]);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || count == 0 || count > 1000000) {
      std::cerr << "operations must be 1..1000000\n"; return 64;
    }
  }
  using Clock = std::chrono::steady_clock;
  const auto directory = std::filesystem::temp_directory_path() /
      ("itoukv-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
  bool owned = false;
  try {
    owned = std::filesystem::create_directory(directory);
    if (!owned) throw itoukv::Error("benchmark temporary directory already exists");
    const auto elapsed = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    double write_ms, read_ms, p50, p95;
    std::uint64_t wal_bytes, live_bytes;
    const std::string payload(128, 'v');
    {
      itoukv::Store store(directory);
      std::vector<double> latency;
      const auto begin = Clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        const auto start = Clock::now();
        store.put("key-" + std::to_string(i), payload);
        latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
      }
      write_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
      std::sort(latency.begin(), latency.end());
      p50 = latency[(count - 1) / 2];
      p95 = latency[static_cast<std::size_t>(static_cast<double>(count - 1) * 0.95)];
      const auto read_begin = Clock::now();
      for (std::size_t i = 0; i < count; ++i)
        if (store.get("key-" + std::to_string(i)) != payload) throw itoukv::Error("benchmark read mismatch");
      read_ms = std::chrono::duration<double, std::milli>(Clock::now() - read_begin).count();
      wal_bytes = store.stats().wal_bytes;
      live_bytes = store.stats().live_bytes;
    }
    const auto wal_recovery_begin = Clock::now();
    auto store = std::make_unique<itoukv::Store>(directory);
    const auto wal_recovery_ms = elapsed(wal_recovery_begin);
    if (store->stats().live_keys != count) throw itoukv::Error("WAL recovery mismatch");
    const auto compact_begin = Clock::now();
    store->compact();
    const auto compact_ms = elapsed(compact_begin);
    const auto lookup_begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
      if (store->get("key-" + std::to_string(i)) != payload) throw itoukv::Error("SSTable read mismatch");
    const auto sstable_lookup_ms = elapsed(lookup_begin);
    const auto negatives_before = store->stats().bloom_negatives;
    const auto missing_begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
      if (store->get("missing-" + std::to_string(i))) throw itoukv::Error("missing key returned value");
    const auto missing_ms = elapsed(missing_begin);
    const auto state = store->stats();
    const auto rejected = state.bloom_negatives - negatives_before;
    store.reset();
    const auto sstable_recovery_begin = Clock::now();
    store = std::make_unique<itoukv::Store>(directory);
    const auto sstable_recovery_ms = elapsed(sstable_recovery_begin);
    if (store->stats().live_keys != count || store->stats().memtable_entries != 0)
      throw itoukv::Error("SSTable recovery mismatch");
    store.reset();
    std::cout << "{\"operations\":" << count << ",\"value_bytes\":128,\"sync_per_write\":true"
              << ",\"write_ms\":" << write_ms << ",\"write_ops_per_second\":" << count * 1000.0 / write_ms
              << ",\"write_p50_ms\":" << p50 << ",\"write_p95_ms\":" << p95
              << ",\"memtable_read_ms\":" << read_ms << ",\"wal_recovery_ms\":" << wal_recovery_ms
              << ",\"compact_ms\":" << compact_ms << ",\"sstable_lookup_ms\":" << sstable_lookup_ms
              << ",\"sstable_recovery_ms\":" << sstable_recovery_ms << ",\"missing_lookup_ms\":" << missing_ms
              << ",\"bloom_negatives\":" << rejected << ",\"bloom_false_positives\":" << count - rejected
              << ",\"table_rows_read\":" << state.table_rows_read
              << ",\"logical_live_bytes\":" << live_bytes << ",\"initial_wal_bytes\":" << wal_bytes
              << ",\"sstable_bytes\":" << state.sstable_bytes
              << ",\"approx_write_amplification\":" << static_cast<double>(wal_bytes + state.sstable_bytes + 32) / static_cast<double>(live_bytes)
              << ",\"warm_file_cache\":true}\n";
    std::filesystem::remove_all(directory);
    return 0;
  } catch (const std::exception& error) {
    std::error_code ec;
    if (owned) std::filesystem::remove_all(directory, ec);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
