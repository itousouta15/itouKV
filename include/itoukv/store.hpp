#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace itoukv {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
class Corruption : public Error {
 public:
  using Error::Error;
};
class Locked : public Error {
 public:
  using Error::Error;
};

// Explicit test instrumentation. The CLI never installs a fault hook.
enum class FaultPoint {
  before_append, after_header, after_batch_entry, after_payload, after_append, after_sync,
  after_sstable_sync, after_new_wal_sync, after_manifest_temp_sync,
  after_manifest_publish, after_compaction_cleanup
};
struct Options {
  std::function<void(FaultPoint)> fault_hook;
};

struct Stats {
  std::uint64_t sequence;
  std::uint64_t wal_bytes;
  std::uint64_t live_keys;
  std::uint64_t live_bytes;
  std::uint64_t recovered_tail_bytes;
  std::uint64_t generation;
  std::uint64_t checkpoint_sequence;
  std::uint64_t sstable_bytes;
  std::uint64_t sstable_keys;
  std::uint64_t memtable_entries;
  std::uint64_t bloom_checks;
  std::uint64_t bloom_negatives;
  std::uint64_t table_rows_read;
};

// Ordered, owned mutations. Repeated keys are applied in insertion order.
// Building a batch performs no I/O; a rejected addition leaves it unchanged.
class WriteBatch {
 public:
  static constexpr std::size_t max_operations = 4096;
  static constexpr std::size_t max_payload_bytes = 32 * 1024 * 1024;
  struct Operation {
    bool deletion;
    std::string key;
    std::string value;
  };
  WriteBatch() = default;
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch& other);
  WriteBatch(WriteBatch&& other) noexcept;
  WriteBatch& operator=(WriteBatch&& other) noexcept;
  WriteBatch& put(std::string_view key, std::string_view value);
  WriteBatch& erase(std::string_view key);
  void clear() noexcept;
  std::size_t size() const noexcept { return operations_.size(); }
  bool empty() const noexcept { return operations_.empty(); }

 private:
  friend class Store;
  std::vector<Operation> operations_;
  std::size_t payload_bytes_ = 0;
  WriteBatch& add(bool deletion, std::string_view key, std::string_view value);
};

// One process/Store owns a directory; calls on that Store are thread-safe.
// Keys and values are arbitrary bytes. Empty keys and empty values are valid.
class Store {
 public:
  static constexpr std::size_t max_key_bytes = 64 * 1024;
  static constexpr std::size_t max_value_bytes = 16 * 1024 * 1024;

  explicit Store(const std::filesystem::path& directory, Options options = {});
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  // Success means the WAL was flushed before publishing the in-memory change.
  // An exception after append begins has an UNKNOWN commit outcome: reopen.
  void put(std::string_view key, std::string_view value);
  bool erase(std::string_view key);
  // One WAL record and one durable flush for the complete batch. Empty is a
  // no-op. Sequence advances once per non-empty batch, not per operation.
  void write(const WriteBatch& batch);
  std::optional<std::string> get(std::string_view key) const;
  // Inclusive start, unsigned-byte lexicographic order, copied snapshot.
  std::vector<std::pair<std::string, std::string>> scan(
      std::string_view start = {}, std::size_t limit = 100) const;
  // Merge the current table and MemTable into one immutable SSTable, then
  // atomically publish a new manifest + empty WAL. Synchronous under the mutex.
  void compact();
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace itoukv
