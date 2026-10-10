#include "itoukv/store.hpp"

#include <charconv>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void usage() {
  std::cout << "itouKV 0.3.0 - durable byte-key/value store\n\n"
               "Usage: itoukv [--hex] <directory> <command> [arguments]\n"
               "  put <key> <value>        Flush a value to the WAL\n"
               "  get <key>                Read a value (exit 2 if absent)\n"
               "  delete <key>             Durable deletion (exit 2 if absent)\n"
               "  scan [start] [limit]     Inclusive byte-sorted scan; default limit 100\n"
               "  compact                  Merge into SSTable and rotate the WAL\n"
               "  batch <file|->           Atomically commit a hex TSV batch (or stdin)\n"
               "  stats                    JSON storage statistics\n\n"
               "--hex decodes key/value arguments and hex-encodes get output.\n"
               "scan always returns JSON with key_hex/value_hex for lossless bytes.\n";
}
std::string hex(std::string_view input) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(input.size() * 2);
  for (unsigned char c : input) {
    result.push_back(digits[c >> 4]);
    result.push_back(digits[c & 15]);
  }
  return result;
}
std::string unhex(std::string_view input) {
  if (input.size() % 2 != 0) throw itoukv::Error("hex input must contain an even number of digits");
  const auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    throw itoukv::Error("invalid hex digit");
  };
  std::string output;
  output.reserve(input.size() / 2);
  for (std::size_t i = 0; i < input.size(); i += 2)
    output.push_back(static_cast<char>((nibble(input[i]) << 4) | nibble(input[i + 1])));
  return output;
}
itoukv::WriteBatch parse_batch(std::istream& input) {
  constexpr std::size_t max_text = itoukv::WriteBatch::max_payload_bytes * 2 + itoukv::WriteBatch::max_operations * 16;
  std::string text;
  std::array<char, 16384> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    const auto bytes = static_cast<std::size_t>(input.gcount());
    if (bytes > max_text - text.size()) throw itoukv::Error("batch input exceeds text size limit");
    text.append(buffer.data(), bytes);
  }
  if (input.bad() || (!input.eof() && input.fail())) throw itoukv::Error("cannot read batch input");
  itoukv::WriteBatch batch;
  std::size_t start = 0, line_number = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + start, end - start);
    start = end + 1; ++line_number;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    try {
      const auto first = line.find('\t');
      if (first == std::string_view::npos) throw itoukv::Error("expected tab-separated operation and hex key");
      const auto operation = line.substr(0, first);
      const auto second = line.find('\t', first + 1);
      const auto key_text = line.substr(first + 1, second == std::string_view::npos ? second : second - first - 1);
      if (key_text.size() > itoukv::Store::max_key_bytes * 2) throw itoukv::Error("key exceeds 64 KiB limit");
      if (operation == "delete" && second == std::string_view::npos) batch.erase(unhex(key_text));
      else if (operation == "put" && second != std::string_view::npos && line.find('\t', second + 1) == std::string_view::npos) {
        const auto value = line.substr(second + 1);
        if (value.size() > itoukv::Store::max_value_bytes * 2) throw itoukv::Error("value exceeds 16 MiB limit");
        batch.put(unhex(key_text), unhex(value));
      } else throw itoukv::Error("expected put<TAB>key_hex<TAB>value_hex or delete<TAB>key_hex");
    } catch (const itoukv::Error& error) {
      throw itoukv::Error("batch line " + std::to_string(line_number) + ": " + error.what());
    }
  }
  return batch;
}
int run(std::vector<std::string> args) {
  if (args.empty() || args[0] == "--help" || args[0] == "-h") { usage(); return 0; }
  bool binary = false;
  if (args[0] == "--hex") { binary = true; args.erase(args.begin()); }
  if (args.size() < 2) { usage(); return 64; }
  const auto& command = args[1];
  const auto count = args.size() - 2;
  if (!((command == "put" && count == 2) ||
        ((command == "get" || command == "delete") && count == 1) ||
        (command == "batch" && count == 1) ||
        (command == "scan" && count <= 2) || ((command == "stats" || command == "compact") && count == 0))) {
    usage(); return 64;
  }
  try {
    const auto decode = [binary](const std::string& value) { return binary ? unhex(value) : value; };
    std::string key = count > 0 && command != "batch" ? decode(args[2]) : "";
    std::string value = command == "put" ? decode(args[3]) : "";
    std::size_t limit = 100;
    if (command == "scan" && count == 2) {
      const auto& text = args[3];
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || limit > 1000000)
        throw itoukv::Error("scan limit must be an integer between 0 and 1000000");
    }
    itoukv::WriteBatch batch;
    if (command == "batch") {
      if (args[2] == "-") batch = parse_batch(std::cin);
      else {
        std::ifstream file(std::filesystem::u8path(args[2]), std::ios::binary);
        if (!file) throw itoukv::Error("cannot open batch file");
        batch = parse_batch(file);
      }
    }
    itoukv::Store store(std::filesystem::u8path(args[0]));
    if (command == "put") {
      store.put(key, value);
      std::cout << "OK\n";
    } else if (command == "get") {
      const auto found = store.get(key);
      if (!found) { std::cerr << "key not found\n"; return 2; }
      std::cout << (binary ? hex(*found) : *found) << '\n';
    } else if (command == "delete") {
      const bool existed = store.erase(key);
      std::cout << (existed ? "DELETED\n" : "NOT_FOUND\n");
      return existed ? 0 : 2;
    } else if (command == "scan") {
      std::cout << '[';
      bool first = true;
      for (const auto& [k, v] : store.scan(key, limit)) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"key_hex\":\"" << hex(k) << "\",\"value_hex\":\"" << hex(v) << "\"}";
      }
      std::cout << "]\n";
    } else if (command == "batch") {
      store.write(batch);
      std::cout << "COMMITTED " << batch.size() << '\n';
    } else if (command == "compact") {
      store.compact();
      std::cout << "COMPACTED\n";
    } else {
      const auto s = store.stats();
      std::cout << "{\"sequence\":" << s.sequence << ",\"wal_bytes\":" << s.wal_bytes
                << ",\"live_keys\":" << s.live_keys << ",\"live_bytes\":" << s.live_bytes
                << ",\"recovered_tail_bytes\":" << s.recovered_tail_bytes
                << ",\"generation\":" << s.generation << ",\"checkpoint_sequence\":" << s.checkpoint_sequence
                << ",\"sstable_bytes\":" << s.sstable_bytes << ",\"sstable_keys\":" << s.sstable_keys
                << ",\"memtable_entries\":" << s.memtable_entries << ",\"bloom_checks\":" << s.bloom_checks
                << ",\"bloom_negatives\":" << s.bloom_negatives << ",\"table_rows_read\":" << s.table_rows_read << "}\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "itoukv: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  _setmode(_fileno(stdout), _O_BINARY);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return 64;
    std::string text(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), bytes, nullptr, nullptr);
    text.pop_back();
    args.push_back(std::move(text));
  }
  return run(std::move(args));
}
#else
int main(int argc, char* argv[]) { return run({argv + 1, argv + argc}); }
#endif
