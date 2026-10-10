#include "itoukv/store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace itoukv {
namespace {

constexpr std::size_t header_size = 32;
constexpr std::uint8_t put_tag = 1;
constexpr std::uint8_t delete_tag = 2;
constexpr std::uint8_t batch_tag = 3;
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void io_error(const char* action) {
#ifdef _WIN32
  const auto code = static_cast<int>(GetLastError());
  throw Error(std::string(action) + ": " + std::system_category().message(code));
#else
  const int code = errno;
  throw Error(std::string(action) + ": " + std::generic_category().message(code));
#endif
}

// A deliberately small OS layer: positioned reads/writes, durable flush,
// truncation and an OS-released exclusive lock (no stale PID-file protocol).
class File {
 public:
  explicit File(const std::filesystem::path& path, bool exclusive_lock = false, bool must_exist = false) {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, must_exist ? OPEN_EXISTING : OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) io_error("open");
    if (exclusive_lock) {
      OVERLAPPED overlapped{};
      if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0, MAXDWORD, MAXDWORD, &overlapped)) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        throw Locked("database is already open (exclusive LOCK unavailable)");
      }
    }
#else
    fd_ = ::open(path.c_str(), O_RDWR | (must_exist ? 0 : O_CREAT) | O_CLOEXEC, 0600);
    if (fd_ < 0) io_error("open");
    if (exclusive_lock && ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      fd_ = -1;
      throw Locked("database is already open (exclusive LOCK unavailable)");
    }
#endif
  }
  ~File() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  std::uint64_t size() const {
#ifdef _WIN32
    LARGE_INTEGER result{};
    if (!GetFileSizeEx(handle_, &result)) io_error("file size");
    return static_cast<std::uint64_t>(result.QuadPart);
#else
    struct stat result {};
    if (::fstat(fd_, &result) != 0) io_error("file size");
    return static_cast<std::uint64_t>(result.st_size);
#endif
  }

  void read(std::uint64_t offset, std::span<std::uint8_t> bytes) const {
    std::size_t done = 0;
    while (done < bytes.size()) {
#ifdef _WIN32
      seek(offset + done);
      DWORD count = 0;
      const auto requested = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1024 * 1024));
      if (!ReadFile(handle_, bytes.data() + done, requested, &count, nullptr))
        io_error("read");
#else
      const auto count = ::pread(fd_, bytes.data() + done, std::min<std::size_t>(bytes.size() - done, 1024 * 1024),
                                 static_cast<off_t>(offset + done));
      if (count < 0) {
        if (errno == EINTR) continue;
        io_error("read");
      }
#endif
      if (count == 0) throw Error("unexpected EOF during read");
      done += static_cast<std::size_t>(count);
    }
  }

  void write(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
#ifdef _WIN32
      seek(offset + done);
      DWORD count = 0;
      const auto requested = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1024 * 1024));
      if (!WriteFile(handle_, bytes.data() + done, requested, &count, nullptr))
        io_error("write");
#else
      const auto count = ::pwrite(fd_, bytes.data() + done, std::min<std::size_t>(bytes.size() - done, 1024 * 1024),
                                  static_cast<off_t>(offset + done));
      if (count < 0) {
        if (errno == EINTR) continue;
        io_error("write");
      }
#endif
      if (count == 0) throw Error("write made no progress");
      done += static_cast<std::size_t>(count);
    }
  }

  void sync() {
#ifdef _WIN32
    if (!FlushFileBuffers(handle_)) io_error("FlushFileBuffers");
#else
    int result;
    do {
      result = ::fsync(fd_);
    } while (result != 0 && errno == EINTR);
    if (result != 0) io_error("fsync");
#endif
  }

  void truncate(std::uint64_t length) {
#ifdef _WIN32
    seek(length);
    if (!SetEndOfFile(handle_)) io_error("truncate");
#else
    if (::ftruncate(fd_, static_cast<off_t>(length)) != 0) io_error("truncate");
#endif
    sync();
  }

 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  void seek(std::uint64_t offset) const {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) io_error("seek");
  }
#else
  int fd_ = -1;
#endif
};

void sync_directory(const std::filesystem::path& directory) {
#ifndef _WIN32
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) io_error("open directory for sync");
  int result;
  do {
    result = ::fsync(fd);
  } while (result != 0 && errno == EINTR);
  const int saved = errno;
  ::close(fd);
  if (result != 0) {
    errno = saved;
    io_error("directory fsync");
  }
#else
  // Windows has no generally supported POSIX-style directory fsync. See the
  // documented new-directory/power-loss boundary; file data is still flushed.
  (void)directory;
#endif
}

std::filesystem::path prepare_directory(const std::filesystem::path& path) {
  if (path.empty()) throw Error("database directory must not be empty");
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  // Create one component at a time so each new directory entry can be synced.
  std::vector<std::filesystem::path> missing;
  auto current = absolute;
  while (!std::filesystem::exists(current)) {
    missing.push_back(current);
    const auto parent = current.parent_path();
    if (parent == current) throw Error("cannot find existing parent directory");
    current = parent;
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    std::filesystem::create_directory(*it);
    sync_directory(it->parent_path());
  }
  if (!std::filesystem::is_directory(absolute)) throw Error("database path is not a directory");
  return absolute;
}

std::uint32_t crc32_extend(std::uint32_t value, std::span<const std::uint8_t> bytes) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      auto value = i;
      for (int bit = 0; bit < 8; ++bit)
        value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
      result[i] = value;
    }
    return result;
  }();
  for (const auto byte : bytes) value = table[(value ^ byte) & 0xffU] ^ (value >> 8U);
  return value;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  return crc32_extend(0xffffffffU, bytes) ^ 0xffffffffU;
}

std::uint32_t file_checksum(const File& file, std::uint64_t length) {
  std::array<std::uint8_t, 65536> buffer{};
  std::uint32_t state = 0xffffffffU;
  std::uint64_t offset = 0;
  while (offset < length) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - offset));
    const auto bytes = std::span(buffer).first(count);
    file.read(offset, bytes);
    state = crc32_extend(state, bytes);
    offset += count;
  }
  return state ^ 0xffffffffU;
}

void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
void put_u64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8U);
  return value;
}
std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
  return value;
}

struct ByteLess {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
  }
};

Bytes encode(std::uint64_t sequence, std::uint8_t tag, std::string_view key, std::string_view value) {
  Bytes bytes(header_size + key.size() + value.size() + 4, 0);
  bytes[0] = 'I'; bytes[1] = 'K'; bytes[2] = 'V'; bytes[3] = '1';
  put_u32(bytes, 4, static_cast<std::uint32_t>(key.size() + value.size()));
  put_u64(bytes, 8, sequence);
  put_u32(bytes, 16, static_cast<std::uint32_t>(key.size()));
  put_u32(bytes, 20, static_cast<std::uint32_t>(value.size()));
  bytes[24] = tag;
  put_u32(bytes, 28, crc32(std::span(bytes).first(28)));
  if (!key.empty()) std::memcpy(bytes.data() + header_size, key.data(), key.size());
  if (!value.empty()) std::memcpy(bytes.data() + header_size + key.size(), value.data(), value.size());
  put_u32(bytes, bytes.size() - 4, crc32(std::span(bytes).first(bytes.size() - 4)));
  return bytes;
}

Bytes encode_batch(std::uint64_t sequence, std::span<const WriteBatch::Operation> operations,
                   std::size_t payload_size, std::vector<std::size_t>& boundaries) {
  Bytes bytes(header_size + payload_size + 4, 0);
  std::memcpy(bytes.data(), "IKV2", 4);
  put_u32(bytes, 4, static_cast<std::uint32_t>(payload_size));
  put_u64(bytes, 8, sequence);
  put_u32(bytes, 16, static_cast<std::uint32_t>(operations.size()));
  bytes[24] = batch_tag;
  put_u32(bytes, 28, crc32(std::span(bytes).first(28)));
  std::size_t cursor = header_size;
  for (const auto& operation : operations) {
    bytes[cursor] = operation.deletion ? delete_tag : put_tag;
    put_u32(bytes, cursor + 1, static_cast<std::uint32_t>(operation.key.size()));
    put_u32(bytes, cursor + 5, static_cast<std::uint32_t>(operation.value.size()));
    cursor += 9;
    if (!operation.key.empty()) std::memcpy(bytes.data() + cursor, operation.key.data(), operation.key.size());
    cursor += operation.key.size();
    if (!operation.value.empty()) std::memcpy(bytes.data() + cursor, operation.value.data(), operation.value.size());
    cursor += operation.value.size();
    boundaries.push_back(cursor);
  }
  put_u32(bytes, cursor, crc32(std::span(bytes).first(cursor)));
  return bytes;
}

constexpr std::uint64_t table_header_size = 80;
constexpr std::uint64_t index_stride = 16;

class Bloom {
 public:
  explicit Bloom(std::uint64_t entries)
      : bits(static_cast<std::size_t>((std::max<std::uint64_t>(64, entries * 10) + 7) / 8), 0) {}
  Bytes bits;
  void add(std::string_view key) {
    positions(key, [&](std::uint64_t bit) { bits[static_cast<std::size_t>(bit / 8)] |= static_cast<std::uint8_t>(1U << (bit % 8)); });
  }
  bool may_contain(std::string_view key) const {
    bool result = true;
    positions(key, [&](std::uint64_t bit) {
      if ((bits[static_cast<std::size_t>(bit / 8)] & (1U << (bit % 8))) == 0) result = false;
    });
    return result;
  }
 private:
  template <typename Fn>
  void positions(std::string_view key, Fn&& fn) const {
    std::uint64_t first = 14695981039346656037ULL;
    for (unsigned char byte : key) { first ^= byte; first *= 1099511628211ULL; }
    std::uint64_t second = first ^ 0x9e3779b97f4a7c15ULL;
    second = (second ^ (second >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    second = (second ^ (second >> 27U)) * 0x94d049bb133111ebULL;
    second = (second ^ (second >> 31U)) | 1ULL;
    const auto count = static_cast<std::uint64_t>(bits.size()) * 8;
    for (unsigned i = 0; i < 7; ++i) fn((first + i * second) % count);
  }
};

class Table {
  struct IndexEntry { std::string key; std::uint64_t offset; };
  struct Row { std::string key; std::string value; std::uint64_t next; };
  File file_;
  std::uint64_t data_end_ = 0;
  std::vector<IndexEntry> index_;
  Bloom bloom_{0};

  [[noreturn]] static void corrupt(const char* why) { throw Corruption(std::string("SSTable corruption: ") + why); }
  std::uint64_t locate(std::string_view key) const {
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
        [](std::string_view wanted, const IndexEntry& entry) { return ByteLess{}(wanted, entry.key); });
    return it == index_.begin() ? table_header_size : std::prev(it)->offset;
  }
  Row read_row(std::uint64_t offset) const {
    if (offset > data_end_ || data_end_ - offset < 12) corrupt("truncated row");
    std::array<std::uint8_t, 8> header{};
    file_.read(offset, header);
    const auto key_size = get_u32(header, 0), value_size = get_u32(header, 4);
    if (key_size > Store::max_key_bytes || value_size > Store::max_value_bytes) corrupt("row lengths out of bounds");
    const auto length = static_cast<std::uint64_t>(key_size) + value_size + 12;
    if (length > data_end_ - offset) corrupt("row crosses data boundary");
    Bytes bytes(static_cast<std::size_t>(length));
    std::copy(header.begin(), header.end(), bytes.begin());
    file_.read(offset + 8, std::span(bytes).subspan(8));
    if (get_u32(bytes, bytes.size() - 4) != crc32(std::span(bytes).first(bytes.size() - 4))) corrupt("row checksum mismatch");
    ++rows_read;
    return {std::string(reinterpret_cast<const char*>(bytes.data() + 8), key_size),
            std::string(reinterpret_cast<const char*>(bytes.data() + 8 + key_size), value_size), offset + length};
  }

 public:
  std::uint64_t sequence = 0, entries = 0, logical_bytes = 0, file_bytes = 0;
  mutable std::uint64_t bloom_checks = 0, bloom_negatives = 0, rows_read = 0;

  explicit Table(const std::filesystem::path& path) : file_(path, false, true) {
    file_bytes = file_.size();
    if (file_bytes < table_header_size + 4) corrupt("truncated file");
    std::array<std::uint8_t, table_header_size> header{};
    file_.read(0, header);
    if (std::memcmp(header.data(), "IKVSST01", 8) != 0) corrupt("bad magic/version");
    if (get_u32(header, 76) != crc32(std::span(header).first(76))) corrupt("header checksum mismatch");
    for (std::size_t i = 64; i < 76; ++i) if (header[i] != 0) corrupt("nonzero reserved fields");
    sequence = get_u64(header, 8);
    entries = get_u64(header, 16);
    data_end_ = get_u64(header, 24);
    const auto bloom_offset = get_u64(header, 32), bloom_bytes = get_u64(header, 40);
    const auto index_count = get_u64(header, 48);
    logical_bytes = get_u64(header, 56);
    if (data_end_ < table_header_size || data_end_ > bloom_offset || bloom_offset > file_bytes - 4 ||
        bloom_bytes != file_bytes - 4 - bloom_offset || entries > (data_end_ - table_header_size) / 12 ||
        logical_bytes != data_end_ - table_header_size - entries * 12 ||
        index_count != (entries + index_stride - 1) / index_stride ||
        bloom_bytes != (std::max<std::uint64_t>(64, entries * 10) + 7) / 8)
      corrupt("invalid metadata/boundaries");
    std::array<std::uint8_t, 4> checksum{};
    file_.read(file_bytes - 4, checksum);
    if (get_u32(checksum, 0) != file_checksum(file_, file_bytes - 4)) corrupt("file checksum mismatch");
    std::uint64_t offset = data_end_;
    for (std::uint64_t i = 0; i < index_count; ++i) {
      if (bloom_offset - offset < 4) corrupt("truncated index length");
      std::array<std::uint8_t, 4> length{};
      file_.read(offset, length);
      const auto key_size = get_u32(length, 0);
      if (key_size > Store::max_key_bytes || bloom_offset - offset < static_cast<std::uint64_t>(key_size) + 12)
        corrupt("invalid index entry length");
      Bytes item(static_cast<std::size_t>(key_size) + 8);
      file_.read(offset + 4, item);
      IndexEntry entry{std::string(reinterpret_cast<const char*>(item.data()), key_size), get_u64(item, key_size)};
      if (entry.offset < table_header_size || entry.offset >= data_end_ ||
          (i == 0 && entry.offset != table_header_size) ||
          (i > 0 && (!ByteLess{}(index_.back().key, entry.key) || index_.back().offset >= entry.offset)))
        corrupt("unordered/invalid sparse index");
      index_.push_back(std::move(entry));
      offset += static_cast<std::uint64_t>(key_size) + 12;
    }
    if (offset != bloom_offset) corrupt("index size mismatch");
    bloom_ = Bloom(entries);
    file_.read(bloom_offset, bloom_.bits);
  }

  std::optional<std::string> get(std::string_view key) const {
    ++bloom_checks;
    if (!bloom_.may_contain(key)) { ++bloom_negatives; return std::nullopt; }
    auto offset = locate(key);
    while (offset < data_end_) {
      auto row = read_row(offset);
      if (row.key == key) return std::move(row.value);
      if (ByteLess{}(key, row.key)) return std::nullopt;
      offset = row.next;
    }
    return std::nullopt;
  }

  class Cursor {
   public:
    std::optional<std::pair<std::string, std::string>> current;
    Cursor(const Table* table, std::string_view start) : table_(table), offset_(table ? table->locate(start) : 0) {
      advance();
      while (current && ByteLess{}(current->first, start)) advance();
    }
    void advance() {
      if (!table_ || offset_ == table_->data_end_) { current.reset(); return; }
      auto row = table_->read_row(offset_);
      offset_ = row.next;
      current.emplace(std::move(row.key), std::move(row.value));
    }
   private:
    const Table* table_;
    std::uint64_t offset_;
  };

  using Sink = std::function<void(std::string_view, std::string_view)>;
  using Producer = std::function<void(const Sink&)>;
  static void write(const std::filesystem::path& path, std::uint64_t sequence,
                    std::uint64_t expected_entries, std::uint64_t expected_bytes, const Producer& produce) {
    File file(path);
    file.truncate(0);
    Bytes header(table_header_size, 0);
    file.write(0, header);
    Bloom bloom(expected_entries);
    std::vector<IndexEntry> index;
    std::uint64_t offset = table_header_size, count = 0, logical = 0;
    std::optional<std::string> previous;
    produce([&](std::string_view key, std::string_view value) {
      if (key.size() > Store::max_key_bytes || value.size() > Store::max_value_bytes ||
          (previous && !ByteLess{}(*previous, key))) throw Error("invalid sorted input during compaction");
      if (count % index_stride == 0) index.push_back({std::string(key), offset});
      Bytes row(key.size() + value.size() + 12, 0);
      put_u32(row, 0, static_cast<std::uint32_t>(key.size()));
      put_u32(row, 4, static_cast<std::uint32_t>(value.size()));
      if (!key.empty()) std::memcpy(row.data() + 8, key.data(), key.size());
      if (!value.empty()) std::memcpy(row.data() + 8 + key.size(), value.data(), value.size());
      put_u32(row, row.size() - 4, crc32(std::span(row).first(row.size() - 4)));
      file.write(offset, row);
      offset += row.size();
      ++count;
      logical += key.size() + value.size();
      previous = key;
      bloom.add(key);
    });
    if (count != expected_entries || logical != expected_bytes) throw Error("compaction live-state accounting mismatch");
    const auto index_offset = offset;
    for (const auto& item : index) {
      Bytes bytes(item.key.size() + 12, 0);
      put_u32(bytes, 0, static_cast<std::uint32_t>(item.key.size()));
      if (!item.key.empty()) std::memcpy(bytes.data() + 4, item.key.data(), item.key.size());
      put_u64(bytes, 4 + item.key.size(), item.offset);
      file.write(offset, bytes);
      offset += bytes.size();
    }
    const auto bloom_offset = offset;
    file.write(offset, bloom.bits);
    offset += bloom.bits.size();
    std::memcpy(header.data(), "IKVSST01", 8);
    put_u64(header, 8, sequence); put_u64(header, 16, count); put_u64(header, 24, index_offset);
    put_u64(header, 32, bloom_offset); put_u64(header, 40, bloom.bits.size());
    put_u64(header, 48, index.size()); put_u64(header, 56, logical);
    put_u32(header, 76, crc32(std::span(header).first(76)));
    file.write(0, header);
    Bytes checksum(4, 0);
    put_u32(checksum, 0, file_checksum(file, offset));
    file.write(offset, checksum);
    file.sync();
  }
};

void atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) io_error("publish manifest");
#else
  if (::rename(from.c_str(), to.c_str()) != 0) io_error("publish manifest");
#endif
}

}  // namespace

WriteBatch::WriteBatch(WriteBatch&& other) noexcept
    : operations_(std::move(other.operations_)), payload_bytes_(std::exchange(other.payload_bytes_, 0)) {
  other.operations_.clear();
}
WriteBatch& WriteBatch::operator=(const WriteBatch& other) {
  if (this != &other) {
    WriteBatch copy(other);
    *this = std::move(copy);
  }
  return *this;
}
WriteBatch& WriteBatch::operator=(WriteBatch&& other) noexcept {
  if (this != &other) {
    operations_ = std::move(other.operations_);
    payload_bytes_ = std::exchange(other.payload_bytes_, 0);
    other.operations_.clear();
  }
  return *this;
}
WriteBatch& WriteBatch::add(bool deletion, std::string_view key, std::string_view value) {
  if (key.size() > Store::max_key_bytes) throw Error("key exceeds 64 KiB limit");
  if (value.size() > Store::max_value_bytes) throw Error("value exceeds 16 MiB limit");
  if (operations_.size() >= max_operations) throw Error("batch exceeds 4096 operations");
  const auto bytes = 9 + key.size() + value.size();
  if (bytes > max_payload_bytes - payload_bytes_) throw Error("batch payload exceeds 32 MiB limit");
  operations_.push_back({deletion, std::string(key), std::string(value)});
  payload_bytes_ += bytes;
  return *this;
}
WriteBatch& WriteBatch::put(std::string_view key, std::string_view value) { return add(false, key, value); }
WriteBatch& WriteBatch::erase(std::string_view key) { return add(true, key, {}); }
void WriteBatch::clear() noexcept { operations_.clear(); payload_bytes_ = 0; }

struct Store::Impl {
  std::filesystem::path directory;
  File lock;
  std::unique_ptr<File> wal;
  std::unique_ptr<Table> table;
  Options options;
  mutable std::mutex mutex;
  std::map<std::string, std::optional<std::string>, ByteLess> data;
  std::uint64_t sequence = 0;
  std::uint64_t wal_bytes = 0;
  std::uint64_t recovered_tail_bytes = 0;
  std::uint64_t generation = 0;
  std::uint64_t checkpoint_sequence = 0;
  std::uint64_t live_keys = 0;
  std::uint64_t live_bytes = 0;
  bool poisoned = false;

  Impl(const std::filesystem::path& path, Options opts)
      : directory(prepare_directory(path)), lock(directory / "LOCK", true),
        options(std::move(opts)) {
    const auto current_path = directory / "CURRENT";
    if (std::filesystem::exists(current_path)) {
      File manifest(current_path, false, true);
      if (manifest.size() != 32) throw Corruption("manifest has invalid length");
      std::array<std::uint8_t, 32> bytes{};
      manifest.read(0, bytes);
      if (std::memcmp(bytes.data(), "IKVCUR01", 8) != 0 || get_u32(bytes, 24) != 0 ||
          get_u32(bytes, 28) != crc32(std::span(bytes).first(28)))
        throw Corruption("manifest checksum/version mismatch");
      generation = get_u64(bytes, 8);
      checkpoint_sequence = get_u64(bytes, 16);
      if (generation == 0) throw Corruption("manifest generation must be positive");
      table = std::make_unique<Table>(table_path(generation));
      if (table->sequence != checkpoint_sequence) throw Corruption("manifest/table sequence mismatch");
      sequence = checkpoint_sequence;
      live_keys = table->entries;
      live_bytes = table->logical_bytes;
    }
    wal = std::make_unique<File>(wal_path(generation), false, generation != 0);
    wal->sync();
    sync_directory(directory);
    recover();
  }

  std::filesystem::path wal_path(std::uint64_t gen) const {
    return directory / (gen == 0 ? "data.wal" : "WAL-" + std::to_string(gen) + ".log");
  }
  std::filesystem::path table_path(std::uint64_t gen) const {
    return directory / ("TABLE-" + std::to_string(gen) + ".sst");
  }

  void healthy() const {
    if (poisoned) throw Error("store is poisoned after a storage failure; close and reopen to recover");
  }

  std::optional<std::string> lookup(std::string_view key) const {
    const auto entry = data.find(key);
    if (entry != data.end()) return entry->second;
    return table ? table->get(key) : std::nullopt;
  }

  void apply(std::uint8_t tag, std::string_view key, std::string_view value) {
    const auto old = lookup(key);
    if (old) { --live_keys; live_bytes -= key.size() + old->size(); }
    if (tag == put_tag) {
      data[std::string(key)] = std::string(value);
      ++live_keys; live_bytes += key.size() + value.size();
    } else data[std::string(key)] = std::nullopt;
  }

  void visit(std::string_view start, std::size_t limit, const Table::Sink& sink) const {
    if (limit == 0) return;
    auto mem = data.lower_bound(start);
    Table::Cursor disk(table.get(), start);
    std::size_t emitted = 0;
    while ((mem != data.end() || disk.current) && emitted < limit) {
      if (mem != data.end() && (!disk.current || ByteLess{}(mem->first, disk.current->first))) {
        if (mem->second) { sink(mem->first, *mem->second); ++emitted; }
        ++mem;
      } else if (disk.current && (mem == data.end() || ByteLess{}(disk.current->first, mem->first))) {
        sink(disk.current->first, disk.current->second); ++emitted;
        disk.advance();
      } else {
        // Same key: the MemTable's value or tombstone always wins.
        if (mem->second) { sink(mem->first, *mem->second); ++emitted; }
        ++mem; disk.advance();
      }
    }
  }

  void recover() {
    const auto file_size = wal->size();
    std::uint64_t offset = 0;
    while (offset < file_size) {
      const auto remaining = file_size - offset;
      if (remaining < header_size) break;  // Incomplete final header.
      std::array<std::uint8_t, header_size> header{};
      wal->read(offset, header);
      const auto corrupt = [offset](const char* why) {
        throw Corruption("WAL corruption at byte " + std::to_string(offset) + ": " + why);
      };
      if (header[0] != 'I' || header[1] != 'K' || header[2] != 'V' || (header[3] != '1' && header[3] != '2'))
        corrupt("bad magic/version");
      if (get_u32(header, 28) != crc32(std::span(header).first(28))) corrupt("header checksum mismatch");
      const bool is_batch = header[3] == '2';
      const auto payload_size = get_u32(header, 4);
      const auto key_size = get_u32(header, 16);
      const auto value_size = get_u32(header, 20);
      const auto tag = header[24];
      if (header[25] != 0 || header[26] != 0 || header[27] != 0) corrupt("invalid reserved fields");
      if (is_batch) {
        if (tag != batch_tag || value_size != 0 || key_size == 0 || key_size > WriteBatch::max_operations ||
            payload_size > WriteBatch::max_payload_bytes || payload_size < static_cast<std::uint64_t>(key_size) * 9)
          corrupt("invalid batch header");
      } else {
        if (key_size > Store::max_key_bytes || value_size > Store::max_value_bytes ||
            static_cast<std::uint64_t>(key_size) + value_size != payload_size) corrupt("invalid lengths");
        if ((tag != put_tag && tag != delete_tag) || (tag == delete_tag && value_size != 0))
          corrupt("invalid operation");
      }
      if (sequence == std::numeric_limits<std::uint64_t>::max() || get_u64(header, 8) != sequence + 1)
        corrupt("non-contiguous sequence");
      const auto record_size = header_size + static_cast<std::uint64_t>(payload_size) + 4;
      if (remaining < record_size) break;  // Valid header, incomplete final body.
      Bytes record(static_cast<std::size_t>(record_size));
      std::copy(header.begin(), header.end(), record.begin());
      wal->read(offset + header_size, std::span(record).subspan(header_size));
      if (get_u32(record, record.size() - 4) != crc32(std::span(record).first(record.size() - 4)))
        corrupt("record checksum mismatch");
      if (is_batch) {
        struct View { std::uint8_t tag; std::string_view key, value; };
        std::vector<View> operations;
        operations.reserve(key_size);
        std::size_t cursor = header_size;
        const auto end = record.size() - 4;
        for (std::uint32_t i = 0; i < key_size; ++i) {
          if (end - cursor < 9) corrupt("truncated batch entry");
          const auto entry_tag = record[cursor];
          const auto key_length = get_u32(record, cursor + 1), value_length = get_u32(record, cursor + 5);
          cursor += 9;
          if (key_length > Store::max_key_bytes || value_length > Store::max_value_bytes ||
              (entry_tag != put_tag && entry_tag != delete_tag) || (entry_tag == delete_tag && value_length != 0) ||
              static_cast<std::uint64_t>(key_length) + value_length > end - cursor)
            corrupt("invalid batch entry");
          const auto* text = reinterpret_cast<const char*>(record.data() + cursor);
          operations.push_back({entry_tag, {text, key_length}, {text + key_length, value_length}});
          cursor += static_cast<std::size_t>(key_length) + value_length;
        }
        if (cursor != end) corrupt("batch count/payload mismatch");
        // Validate every entry before publishing any of the batch to memory.
        for (const auto& operation : operations) apply(operation.tag, operation.key, operation.value);
      } else {
        const std::string_view key(reinterpret_cast<const char*>(record.data() + header_size), key_size);
        const std::string_view value(reinterpret_cast<const char*>(record.data() + header_size + key_size), value_size);
        apply(tag, key, value);
      }
      ++sequence;
      offset += record_size;
    }
    recovered_tail_bytes = file_size - offset;
    if (recovered_tail_bytes != 0) wal->truncate(offset);
    wal_bytes = offset;
  }

  void fault(FaultPoint point) {
    if (options.fault_hook) options.fault_hook(point);
  }

  template <typename Publish>
  void commit(const Bytes& record, Publish&& publish, std::span<const std::size_t> boundaries = {}) {
    if (wal_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - record.size())
      throw Error("WAL size exhausted");
    try {
      fault(FaultPoint::before_append);
      wal->write(wal_bytes, std::span(record).first(header_size));
      fault(FaultPoint::after_header);
      if (boundaries.empty()) {
        wal->write(wal_bytes + header_size,
                  std::span(record).subspan(header_size, record.size() - header_size - 4));
      } else {
        std::size_t cursor = header_size;
        for (const auto end : boundaries) {
          wal->write(wal_bytes + cursor, std::span(record).subspan(cursor, end - cursor));
          cursor = end;
          fault(FaultPoint::after_batch_entry);
        }
      }
      fault(FaultPoint::after_payload);
      wal->write(wal_bytes + record.size() - 4, std::span(record).last(4));
      fault(FaultPoint::after_append);
      wal->sync();
      fault(FaultPoint::after_sync);
      // A memory allocation failure here also poisons the Store: the durable
      // record exists, but this process must not expose stale memory afterward.
      publish();
      ++sequence;
      wal_bytes += record.size();
    } catch (...) {
      poisoned = true;
      throw;
    }
  }

  void mutate(std::uint8_t tag, std::string_view key, std::string_view value) {
    healthy();
    if (key.size() > Store::max_key_bytes) throw Error("key exceeds 64 KiB limit");
    if (value.size() > Store::max_value_bytes) throw Error("value exceeds 16 MiB limit");
    if (sequence == std::numeric_limits<std::uint64_t>::max()) throw Error("sequence exhausted");
    const auto record = encode(sequence + 1, tag, key, value);
    commit(record, [&] { apply(tag, key, value); });
  }

  void write_batch(std::span<const WriteBatch::Operation> operations, std::size_t payload_size) {
    healthy();
    if (operations.empty()) return;
    if (sequence == std::numeric_limits<std::uint64_t>::max()) throw Error("sequence exhausted");
    std::vector<std::size_t> boundaries;
    boundaries.reserve(operations.size());
    const auto record = encode_batch(sequence + 1, operations, payload_size, boundaries);
    commit(record, [&] {
      for (const auto& operation : operations)
        apply(operation.deletion ? delete_tag : put_tag, operation.key, operation.value);
    }, boundaries);
  }

  void compact() {
    healthy();
    if (generation == std::numeric_limits<std::uint64_t>::max()) throw Error("generation exhausted");
    const auto next_generation = generation + 1;
    const auto next_table_path = table_path(next_generation);
    const auto next_wal_path = wal_path(next_generation);
    const auto old_table_path = table_path(generation);
    const auto old_wal_path = wal_path(generation);
    const auto previous_generation = generation;
    try {
      Table::write(next_table_path, sequence, live_keys, live_bytes,
          [&](const Table::Sink& sink) { visit({}, std::numeric_limits<std::size_t>::max(), sink); });
      fault(FaultPoint::after_sstable_sync);
      auto next_table = std::make_unique<Table>(next_table_path);
      auto next_wal = std::make_unique<File>(next_wal_path);
      next_wal->truncate(0);
      sync_directory(directory);
      fault(FaultPoint::after_new_wal_sync);

      const auto temporary_manifest = directory / "CURRENT.next";
      {
        Bytes bytes(32, 0);
        std::memcpy(bytes.data(), "IKVCUR01", 8);
        put_u64(bytes, 8, next_generation);
        put_u64(bytes, 16, sequence);
        put_u32(bytes, 28, crc32(std::span(bytes).first(28)));
        File file(temporary_manifest);
        file.truncate(0);
        file.write(0, bytes);
        file.sync();
      }
      fault(FaultPoint::after_manifest_temp_sync);
      atomic_replace(temporary_manifest, directory / "CURRENT");
      sync_directory(directory);
      fault(FaultPoint::after_manifest_publish);

      // The published manifest is the commit point. Release old handles before
      // cleanup (also required on Windows). Cleanup never decides live state.
      table = std::move(next_table);
      wal = std::move(next_wal);
      data.clear();
      generation = next_generation;
      checkpoint_sequence = sequence;
      wal_bytes = 0;
      std::error_code ignored;
      std::filesystem::remove(old_wal_path, ignored);
      if (previous_generation != 0) std::filesystem::remove(old_table_path, ignored);
      sync_directory(directory);
      fault(FaultPoint::after_compaction_cleanup);
    } catch (...) {
      poisoned = true;
      throw;
    }
  }
};

Store::Store(const std::filesystem::path& directory, Options options)
    : impl_(std::make_unique<Impl>(directory, std::move(options))) {}
Store::~Store() = default;

void Store::put(std::string_view key, std::string_view value) {
  std::lock_guard guard(impl_->mutex);
  impl_->mutate(put_tag, key, value);
}
bool Store::erase(std::string_view key) {
  std::lock_guard guard(impl_->mutex);
  impl_->healthy();
  const bool existed = impl_->lookup(key).has_value();
  impl_->mutate(delete_tag, key, {});
  return existed;
}
void Store::write(const WriteBatch& batch) {
  std::lock_guard guard(impl_->mutex);
  impl_->write_batch(batch.operations_, batch.payload_bytes_);
}
std::optional<std::string> Store::get(std::string_view key) const {
  std::lock_guard guard(impl_->mutex);
  impl_->healthy();
  return impl_->lookup(key);
}
std::vector<std::pair<std::string, std::string>> Store::scan(std::string_view start, std::size_t limit) const {
  std::lock_guard guard(impl_->mutex);
  impl_->healthy();
  std::vector<std::pair<std::string, std::string>> result;
  impl_->visit(start, limit, [&](std::string_view key, std::string_view value) { result.emplace_back(key, value); });
  return result;
}
void Store::compact() {
  std::lock_guard guard(impl_->mutex);
  impl_->compact();
}
Stats Store::stats() const {
  std::lock_guard guard(impl_->mutex);
  impl_->healthy();
  const auto* table = impl_->table.get();
  return {impl_->sequence, impl_->wal_bytes, impl_->live_keys, impl_->live_bytes,
          impl_->recovered_tail_bytes, impl_->generation, impl_->checkpoint_sequence,
          table ? table->file_bytes : 0, table ? table->entries : 0,
          static_cast<std::uint64_t>(impl_->data.size()), table ? table->bloom_checks : 0,
          table ? table->bloom_negatives : 0, table ? table->rows_read : 0};
}

}  // namespace itoukv
