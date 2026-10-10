#include "itoukv/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using itoukv::Store;

namespace {
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(\
    std::string("check failed at line ") + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

struct Temp {
  fs::path path;
  Temp() {
    static std::atomic<unsigned> number = 0;
    path = fs::temp_directory_path() / ("itoukv-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
      std::to_string(number++));
    fs::create_directories(path);
  }
  ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
template <typename Exception = itoukv::Error, typename Fn>
void throws(Fn&& fn) {
  try { fn(); } catch (const Exception&) { return; }
  throw std::runtime_error("expected exception was not thrown");
}
std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_file(const fs::path& path, std::string_view data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  CHECK(output.good());
}

void basic_and_binary() {
  Temp temp;
  const std::string binary_key("\0\xff\x80", 3);
  const std::string binary_value("hello\0world\xff", 12);
  {
    Store store(temp.path / "nested" / "db");
    CHECK(!store.get("missing"));
    store.put("", "");
    store.put("name", "itou");
    store.put("name", "updated");
    store.put(binary_key, binary_value);
    CHECK(store.get("") == "");
    CHECK(store.get("name") == "updated");
    CHECK(store.get(binary_key) == binary_value);
    CHECK(!store.erase("absent"));
    CHECK(store.erase("name"));
    CHECK(!store.get("name"));
    CHECK(store.stats().sequence == 6);
  }
  Store reopened(temp.path / "nested" / "db");
  CHECK(reopened.get(binary_key) == binary_value);
  CHECK(reopened.get("") == "");
  CHECK(!reopened.get("name"));
  CHECK(reopened.stats().sequence == 6);
  CHECK(reopened.stats().live_keys == 2);
  CHECK(reopened.stats().recovered_tail_bytes == 0);
}

void scan_order_and_snapshot() {
  Temp temp;
  Store store(temp.path);
  for (auto key : {std::string("\xff", 1), std::string("a"), std::string("\x80", 1), std::string("")})
    store.put(key, key);
  const auto snapshot = store.scan();
  CHECK(snapshot.size() == 4);
  CHECK(snapshot[0].first.empty());
  CHECK(snapshot[1].first == "a");
  CHECK(snapshot[2].first == std::string("\x80", 1));
  CHECK(snapshot[3].first == std::string("\xff", 1));
  CHECK(store.scan("a", 1).size() == 1);
  CHECK(store.scan("a", 1)[0].first == "a");
  CHECK(store.scan("", 0).empty());
  store.put("a", "changed");
  CHECK(snapshot[1].second == "a");
}

void randomized_reference_model() {
  Temp temp;
  std::mt19937 random(20261010);
  std::map<std::string, std::string> reference;
  auto store = std::make_unique<Store>(temp.path);
  std::uint64_t sequence = 0;
  for (unsigned step = 0; step < 1200; ++step) {
    const auto key = "key-" + std::to_string(random() % 47);
    switch (random() % 4) {
      case 0:
      case 1: {
        std::string value(random() % 90, '\0');
        for (auto& byte : value) byte = static_cast<char>(random() & 255);
        store->put(key, value);
        reference[key] = value;
        ++sequence;
        break;
      }
      case 2: {
        const bool existed = reference.erase(key) != 0;
        CHECK(store->erase(key) == existed);
        ++sequence;
        break;
      }
      default: {
        const auto it = reference.find(key);
        const auto expected = it == reference.end() ? std::nullopt : std::optional(it->second);
        CHECK(store->get(key) == expected);
      }
    }
    if (step % 113 == 0) {
      store->compact();
      CHECK(store->stats().checkpoint_sequence == sequence);
      CHECK(store->stats().wal_bytes == 0);
      CHECK(store->stats().memtable_entries == 0);
    }
    if (step % 37 == 0) {
      store.reset();
      store = std::make_unique<Store>(temp.path);
      CHECK(store->stats().sequence == sequence);
      const auto actual = store->scan("", 1000);
      CHECK(actual.size() == reference.size());
      std::uint64_t live_bytes = 0;
      std::size_t index = 0;
      for (const auto& [k, v] : reference) {
        CHECK(actual[index].first == k && actual[index].second == v);
        live_bytes += k.size() + v.size();
        ++index;
      }
      CHECK(store->stats().live_bytes == live_bytes);
    }
  }
}

void every_truncated_tail_boundary() {
  Temp source;
  std::size_t prefix_size = 0;
  {
    Store store(source.path);
    store.put("stable", "acknowledged");
    prefix_size = static_cast<std::size_t>(store.stats().wal_bytes);
    store.put("pending", "the-last-record");
  }
  const auto wal = read_file(source.path / "data.wal");
  const auto tail_size = wal.size() - prefix_size;
  Temp target;
  for (std::size_t bytes = 0; bytes < tail_size; ++bytes) {
    const auto dir = target.path / std::to_string(bytes);
    fs::create_directory(dir);
    write_file(dir / "data.wal", std::string_view(wal).substr(0, prefix_size + bytes));
    {
      Store recovered(dir);
      CHECK(recovered.get("stable") == "acknowledged");
      CHECK(!recovered.get("pending"));
      CHECK(recovered.stats().sequence == 1);
      CHECK(recovered.stats().recovered_tail_bytes == bytes);
      CHECK(recovered.stats().wal_bytes == prefix_size);
      recovered.put("next", "valid-after-truncate");
    }
    Store again(dir);
    CHECK(again.get("next") == "valid-after-truncate");
    CHECK(again.stats().sequence == 2);
    CHECK(again.stats().recovered_tail_bytes == 0);
  }
}

void corruption_is_not_silently_discarded() {
  Temp source;
  {
    Store store(source.path);
    store.put("first", "one");
    store.put("second", "two");
  }
  const auto original = read_file(source.path / "data.wal");
  Temp target;
  for (std::size_t index = 0; index < original.size(); ++index) {
    auto bytes = original;
    bytes[index] = static_cast<char>(static_cast<unsigned char>(bytes[index]) ^ 1);
    const auto dir = target.path / std::to_string(index);
    fs::create_directory(dir);
    write_file(dir / "data.wal", bytes);
    throws<itoukv::Corruption>([&] { Store broken(dir); });
    CHECK(read_file(dir / "data.wal") == bytes);
  }
  // A complete valid record replayed out of sequence is corruption too.
  const auto dir = target.path / "reordered";
  fs::create_directory(dir);
  write_file(dir / "data.wal", original + original);
  throws<itoukv::Corruption>([&] { Store broken(dir); });
}

void locking_limits_and_failure_poisoning() {
  Temp temp;
  {
    Store store(temp.path);
    throws<itoukv::Locked>([&] { Store second(temp.path); });
    throws([&] { store.put(std::string(Store::max_key_bytes + 1, 'k'), "x"); });
    throws([&] { store.put("x", std::string(Store::max_value_bytes + 1, 'v')); });
    CHECK(store.stats().sequence == 0);
    store.put("valid", "works-after-validation-error");
  }
  {
    Store store(temp.path, {.fault_hook = [](itoukv::FaultPoint point) {
      if (point == itoukv::FaultPoint::after_header) throw itoukv::Error("injected I/O failure");
    }});
    throws([&] { store.put("broken", "partial"); });
    throws([&] { store.get("valid"); });
    throws([&] { store.put("another", "must-not-append-after-partial"); });
    throws([&] { store.stats(); });
  }
  Store recovered(temp.path);
  CHECK(recovered.get("valid") == "works-after-validation-error");
  CHECK(!recovered.get("broken"));
  CHECK(recovered.stats().recovered_tail_bytes == 32);
}

void concurrent_calls() {
  Temp temp;
  {
    Store store(temp.path);
    std::vector<std::thread> threads;
    std::atomic<bool> failed = false;
    for (int worker = 0; worker < 4; ++worker) {
      threads.emplace_back([&, worker] {
        try {
          for (int i = 0; i < 25; ++i) {
            const auto key = std::to_string(worker) + ":" + std::to_string(i);
            store.put(key, "value");
            CHECK(store.get(key) == "value");
          }
        } catch (...) { failed = true; }
      });
    }
    for (auto& thread : threads) thread.join();
    CHECK(!failed);
    CHECK(store.stats().sequence == 100);
  }
  Store reopened(temp.path);
  CHECK(reopened.stats().live_keys == 100);
}

void sstable_bloom_merge_and_compaction() {
  Temp temp;
  std::map<std::string, std::string> reference;
  const auto key_for = [](int i) {
    return "key-" + std::string(4 - std::to_string(i).size(), '0') + std::to_string(i);
  };
  {
    Store store(temp.path);
    for (int i = 0; i < 257; ++i) {
      const auto key = key_for(i), value = "value-" + std::to_string(i);
      store.put(key, value); reference[key] = value;
    }
    store.compact();
    CHECK(store.stats().generation == 1);
    CHECK(store.stats().sstable_keys == 257);
    CHECK(store.stats().memtable_entries == 0);
    CHECK(store.stats().wal_bytes == 0);
    CHECK(!fs::exists(temp.path / "data.wal"));
    for (int i = 0; i < 257; ++i) CHECK(store.get(key_for(i)) == reference[key_for(i)]);
    for (int i = 0; i < 200; ++i) CHECK(!store.get("missing-" + std::to_string(i)));
    CHECK(store.stats().bloom_negatives > 150);
    for (int i = 0; i < 257; i += 7) {
      CHECK(store.erase(key_for(i))); reference.erase(key_for(i));
    }
    store.put("key-0010", "updated"); reference["key-0010"] = "updated";
    store.put("key-0010a", "inserted-between"); reference["key-0010a"] = "inserted-between";
    store.put("", "empty-key"); reference[""] = "empty-key";
    for (const auto& start : {std::string(""), std::string("key-0010"), std::string("key-0010b"), std::string("zz")}) {
      const auto page = store.scan(start, 17);
      std::vector<std::pair<std::string, std::string>> expected;
      for (auto it = reference.lower_bound(start); it != reference.end() && expected.size() < 17; ++it)
        expected.emplace_back(it->first, it->second);
      CHECK(page == expected);
    }
  }
  {
    Store store(temp.path);
    CHECK(store.scan("", 1000).size() == reference.size());
    CHECK(store.get("key-0010") == "updated");
    CHECK(!store.get("key-0000"));
    store.compact();
    CHECK(store.stats().generation == 2);
    CHECK(store.stats().sstable_keys == reference.size());
    CHECK(store.stats().memtable_entries == 0);
    CHECK(!fs::exists(temp.path / "TABLE-1.sst"));
    CHECK(!fs::exists(temp.path / "WAL-1.log"));
    for (const auto& [key, value] : reference) CHECK(store.get(key) == value);
    CHECK(!store.get("key-0000"));  // Compaction must not resurrect tombstones.
    store.put("key-0000", "resurrected-explicitly");
  }
  Store reopened(temp.path);
  CHECK(reopened.get("key-0000") == "resurrected-explicitly");
}

void empty_and_binary_tables() {
  Temp temp;
  {
    Store store(temp.path);
    store.compact();
    CHECK(store.stats().generation == 1);
    CHECK(store.stats().sequence == 0);
    CHECK(store.scan().empty());
    CHECK(!store.get(""));
    store.put("", "");
    store.put(std::string("\0\xff", 2), std::string("\xff\0", 2));
    store.put(std::string("\xff", 1), "last");
    store.compact();
  }
  {
    Store store(temp.path);
    CHECK(store.get("") == "");
    CHECK(store.get(std::string("\0\xff", 2)) == std::string("\xff\0", 2));
    const auto rows = store.scan();
    CHECK(rows.size() == 3 && rows.back().first == std::string("\xff", 1));
    for (const auto& [key, value] : rows) { (void)value; CHECK(store.erase(key)); }
    store.compact();
    CHECK(store.stats().live_keys == 0);
  }
  Store store(temp.path);
  CHECK(store.scan().empty());
  CHECK(!store.get(""));
  CHECK(store.stats().sequence == 6);
}

void table_and_manifest_corruption() {
  Temp source;
  {
    Store store(source.path);
    store.put("a", "one"); store.put("b", "two"); store.compact();
  }
  const auto original_table = read_file(source.path / "TABLE-1.sst");
  const auto manifest = read_file(source.path / "CURRENT");
  Temp target;
  for (std::size_t index = 0; index < original_table.size(); ++index) {
    auto bytes = original_table;
    bytes[index] = static_cast<char>(static_cast<unsigned char>(bytes[index]) ^ 1);
    const auto dir = target.path / ("table-" + std::to_string(index));
    fs::create_directory(dir);
    write_file(dir / "CURRENT", manifest);
    write_file(dir / "TABLE-1.sst", bytes);
    write_file(dir / "WAL-1.log", "");
    throws<itoukv::Corruption>([&] { Store broken(dir); });
    CHECK(read_file(dir / "TABLE-1.sst") == bytes);
  }
  for (std::size_t index = 0; index < manifest.size(); ++index) {
    auto bytes = manifest;
    bytes[index] = static_cast<char>(static_cast<unsigned char>(bytes[index]) ^ 1);
    const auto dir = target.path / ("manifest-" + std::to_string(index));
    fs::create_directory(dir);
    write_file(dir / "CURRENT", bytes);
    throws<itoukv::Corruption>([&] { Store broken(dir); });
  }
  fs::remove(source.path / "WAL-1.log");
  throws([&] { Store missing_wal(source.path); });
  CHECK(!fs::exists(source.path / "WAL-1.log"));  // Never recreate a manifest-referenced WAL.
}

void compaction_exception_is_recoverable() {
  for (const auto point : {itoukv::FaultPoint::after_sstable_sync, itoukv::FaultPoint::after_new_wal_sync,
                          itoukv::FaultPoint::after_manifest_temp_sync, itoukv::FaultPoint::after_manifest_publish,
                          itoukv::FaultPoint::after_compaction_cleanup}) {
    Temp temp;
    {
      Store store(temp.path, {.fault_hook = [point](itoukv::FaultPoint current) {
        if (point == current) throw itoukv::Error("injected compaction failure");
      }});
      store.put("stable", "value");
      throws([&] { store.compact(); });
      throws([&] { store.get("stable"); });
    }
    Store recovered(temp.path);
    CHECK(recovered.get("stable") == "value");
    recovered.compact();
    recovered.put("next", "ok");
    CHECK(recovered.get("next") == "ok");
  }
}

void maximum_size_record_across_io_chunks() {
  Temp temp;
  const std::string key(Store::max_key_bytes, 'k');
  std::string value(Store::max_value_bytes, 'v');
  value[1024 * 1024] = '\0';
  value.back() = static_cast<char>(255);
  {
    Store store(temp.path);
    store.put(key, value);
    CHECK(store.get(key) == value);
  }
  {
    Store store(temp.path);
    CHECK(store.get(key) == value);
    store.compact();
    CHECK(store.get(key) == value);
    CHECK(store.stats().live_bytes == key.size() + value.size());
  }
  Store store(temp.path);
  CHECK(store.get(key) == value);
  CHECK(store.scan(key, 1)[0].second == value);
}

void batch_order_limits_and_move_reuse() {
  Temp temp;
  {
    Store store(temp.path);
    itoukv::WriteBatch empty;
    store.write(empty);
    CHECK(store.stats().sequence == 0 && store.stats().wal_bytes == 0);
    std::string owned = "original";
    itoukv::WriteBatch batch;
    batch.put("left", owned).put("right", "first").erase("right").put("right", "last").erase("absent");
    owned = "changed";
    store.write(batch);
    CHECK(store.stats().sequence == 1);
    CHECK(store.get("left") == "original" && store.get("right") == "last");
    auto copied = batch;
    auto moved = std::move(batch);
    CHECK(batch.empty());
    batch.put("reused", "after-move");
    store.write(batch);
    copied = std::move(moved);
    CHECK(moved.empty());
    moved.put("reused-again", "after-move-assignment");
    store.write(moved);
    itoukv::WriteBatch assigned;
    assigned.put("discarded-by-copy", "old");
    assigned = copied;
    copied.clear();
    store.write(assigned);
    CHECK(!store.get("discarded-by-copy"));
    CHECK(store.stats().sequence == 4);
    store.compact();
    batch.clear();
    CHECK(batch.empty());
    batch.erase("left").put("", std::string("\0\xff", 2));
    store.write(batch);
    CHECK(store.stats().sequence == 5);
  }
  Store store(temp.path);
  CHECK(!store.get("left"));
  CHECK(store.get("right") == "last");
  CHECK(store.get("reused") == "after-move");
  CHECK(store.get("reused-again") == "after-move-assignment");
  CHECK(store.get("") == std::string("\0\xff", 2));
  itoukv::WriteBatch limit;
  for (std::size_t i = 0; i < itoukv::WriteBatch::max_operations; ++i) limit.erase("");
  throws([&] { limit.put("too-many", ""); });
  CHECK(limit.size() == itoukv::WriteBatch::max_operations);
  limit.clear();
  throws([&] { limit.put(std::string(Store::max_key_bytes + 1, 'k'), ""); });
  CHECK(limit.empty());
  const std::string large(Store::max_value_bytes, 'v');
  limit.put("a", large);
  throws([&] { limit.put("b", large); });
  CHECK(limit.size() == 1); // Rejected addition retains the valid first entry.
}

void batch_tail_atomicity_and_corruption() {
  Temp source;
  std::size_t prefix;
  {
    Store store(source.path);
    store.put("left", "old"); store.put("right", "old");
    prefix = static_cast<std::size_t>(store.stats().wal_bytes);
    itoukv::WriteBatch batch;
    batch.put("left", "new").erase("right").put("third", "new");
    store.write(batch);
  }
  const auto full = read_file(source.path / "data.wal");
  const auto bytes = full.size() - prefix;
  Temp target;
  for (std::size_t cut = 0; cut < bytes; ++cut) {
    const auto dir = target.path / ("cut-" + std::to_string(cut));
    fs::create_directory(dir);
    write_file(dir / "data.wal", std::string_view(full).substr(0, prefix + cut));
    Store recovered(dir);
    CHECK(recovered.stats().sequence == 2);
    CHECK(recovered.get("left") == "old" && recovered.get("right") == "old");
    CHECK(!recovered.get("third"));
    CHECK(recovered.stats().recovered_tail_bytes == cut);
    if (cut % 11 == 0) {
      itoukv::WriteBatch next;
      next.put("left", "after").put("right", "after");
      recovered.write(next);
      CHECK(recovered.get("left") == "after" && recovered.get("right") == "after");
    }
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    auto corrupt = full;
    corrupt[prefix + i] = static_cast<char>(static_cast<unsigned char>(corrupt[prefix + i]) ^ 1);
    const auto dir = target.path / ("corrupt-" + std::to_string(i));
    fs::create_directory(dir);
    write_file(dir / "data.wal", corrupt);
    throws<itoukv::Corruption>([&] { Store bad(dir); });
    CHECK(read_file(dir / "data.wal") == corrupt);
  }
  Store complete(source.path);
  CHECK(complete.stats().sequence == 3);
  CHECK(complete.get("left") == "new" && !complete.get("right") && complete.get("third") == "new");
}

void randomized_batch_reference_model() {
  Temp temp;
  std::mt19937 random(0xba7c412);
  std::map<std::string, std::string> reference;
  auto store = std::make_unique<Store>(temp.path);
  for (unsigned transaction = 0; transaction < 300; ++transaction) {
    itoukv::WriteBatch batch;
    const auto count = 1 + random() % 8;
    for (unsigned op = 0; op < count; ++op) {
      const auto key = "key-" + std::to_string(random() % 31);
      if (random() % 3 == 0) { batch.erase(key); reference.erase(key); }
      else {
        std::string value(random() % 32, '\0');
        for (auto& byte : value) byte = static_cast<char>(random() & 255);
        batch.put(key, value); reference[key] = value;
      }
    }
    store->write(batch);
    if (transaction % 31 == 0) store->compact();
    if (transaction % 11 == 0) { store.reset(); store = std::make_unique<Store>(temp.path); }
    CHECK(store->stats().sequence == transaction + 1);
    const auto rows = store->scan("", 1000);
    CHECK(rows.size() == reference.size());
    std::size_t i = 0;
    for (const auto& [key, value] : reference) {
      CHECK(rows[i].first == key && rows[i].second == value); ++i;
    }
  }
}

void batch_visibility_and_poisoning() {
  Temp temp;
  {
    Store store(temp.path, {.fault_hook = [](itoukv::FaultPoint point) {
      if (point == itoukv::FaultPoint::after_batch_entry) std::this_thread::yield();
    }});
    itoukv::WriteBatch initial; initial.put("a", "0").put("b", "0"); store.write(initial);
    std::atomic<bool> done = false, failed = false, started = false;
    std::thread reader([&] {
      try {
        do {
          const auto rows = store.scan();
          if (rows.size() != 2 || rows[0].second != rows[1].second) failed = true;
          started = true;
          std::this_thread::yield();
        } while (!done);
      } catch (...) { failed = true; started = true; }
    });
    while (!started) std::this_thread::yield();
    for (int i = 1; i <= 50; ++i) {
      itoukv::WriteBatch batch;
      batch.put("a", std::to_string(i)).put("b", std::to_string(i)); store.write(batch);
    }
    done = true; reader.join(); CHECK(!failed);
  }
  {
    Store store(temp.path, {.fault_hook = [](itoukv::FaultPoint point) {
      if (point == itoukv::FaultPoint::after_batch_entry) throw itoukv::Error("injected partial batch");
    }});
    itoukv::WriteBatch batch; batch.put("a", "broken").put("b", "broken");
    throws([&] { store.write(batch); });
    throws([&] { store.scan(); });
  }
  Store recovered(temp.path);
  CHECK(recovered.get("a") == "50" && recovered.get("b") == "50");
}

void maximum_payload_batch_roundtrip() {
  Temp temp;
  const std::string first(Store::max_value_bytes, 'a');
  std::string second(Store::max_value_bytes - 20, 'b'); // Two 9-byte entries + two 1-byte keys.
  second.back() = '\0';
  {
    Store store(temp.path);
    itoukv::WriteBatch batch;
    batch.put("a", first).put("b", second);
    throws([&] { batch.erase(""); });
    CHECK(batch.size() == 2);
    store.write(batch);
    CHECK(store.stats().sequence == 1);
    CHECK(store.stats().wal_bytes == itoukv::WriteBatch::max_payload_bytes + 36);
  }
  {
    Store store(temp.path);
    CHECK(store.get("a") == first && store.get("b") == second);
    store.compact();
  }
  Store store(temp.path);
  CHECK(store.get("a") == first && store.get("b") == second);
}

itoukv::FaultPoint parse_point(std::string_view name) {
  if (name == "before_append") return itoukv::FaultPoint::before_append;
  if (name == "after_header") return itoukv::FaultPoint::after_header;
  if (name == "after_batch_entry") return itoukv::FaultPoint::after_batch_entry;
  if (name == "after_payload") return itoukv::FaultPoint::after_payload;
  if (name == "after_append") return itoukv::FaultPoint::after_append;
  if (name == "after_sync") return itoukv::FaultPoint::after_sync;
  if (name == "after_sstable_sync") return itoukv::FaultPoint::after_sstable_sync;
  if (name == "after_new_wal_sync") return itoukv::FaultPoint::after_new_wal_sync;
  if (name == "after_manifest_temp_sync") return itoukv::FaultPoint::after_manifest_temp_sync;
  if (name == "after_manifest_publish") return itoukv::FaultPoint::after_manifest_publish;
  if (name == "after_compaction_cleanup") return itoukv::FaultPoint::after_compaction_cleanup;
  throw std::runtime_error("unknown fault point");
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc == 4 && (std::string_view(argv[1]) == "--crash-worker" || std::string_view(argv[1]) == "--compact-worker" || std::string_view(argv[1]) == "--batch-worker")) {
      const auto target = parse_point(argv[3]);
      Store store(argv[2], {.fault_hook = [target](itoukv::FaultPoint point) {
        if (point == target) std::_Exit(99);  // No destructors, no stdio flush.
      }});
      if (std::string_view(argv[1]) == "--compact-worker") store.compact();
      else if (std::string_view(argv[1]) == "--batch-worker") {
        itoukv::WriteBatch batch;
        batch.put("left", "intermediate").erase("gone").put("right", "new").put("left", "new");
        store.write(batch);
      }
      else store.put("crash", "candidate");
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--hold-lock") {
      Store store(argv[2]);
      std::cout << "LOCKED" << std::endl;
      std::string line;
      std::getline(std::cin, line);
      return 0;
    }
    const std::pair<const char*, void(*)()> tests[] = {
      {"basic and binary persistence", basic_and_binary},
      {"scan byte order and snapshot", scan_order_and_snapshot},
      {"1200 operations against reference model", randomized_reference_model},
      {"every truncated final-record boundary", every_truncated_tail_boundary},
      {"every one-bit corruption and sequence replay", corruption_is_not_silently_discarded},
      {"exclusive lock, limits and poisoned writes", locking_limits_and_failure_poisoning},
      {"concurrent calls and recovery", concurrent_calls},
      {"SSTable sparse index, Bloom filter, merge and compaction", sstable_bloom_merge_and_compaction},
      {"empty and binary SSTables", empty_and_binary_tables},
      {"table and manifest bit flips and missing WAL", table_and_manifest_corruption},
      {"compaction exceptions before and after publication", compaction_exception_is_recoverable},
      {"maximum-size key/value across OS I/O chunks", maximum_size_record_across_io_chunks},
      {"batch ordering, limits, ownership and moved-from reuse", batch_order_limits_and_move_reuse},
      {"every partial batch boundary and bit corruption", batch_tail_atomicity_and_corruption},
      {"300 batches against a reference model", randomized_batch_reference_model},
      {"batch snapshot visibility and poisoned partial writes", batch_visibility_and_poisoning},
      {"exact 32 MiB batch payload survives recovery and compaction", maximum_payload_batch_roundtrip},
    };
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
