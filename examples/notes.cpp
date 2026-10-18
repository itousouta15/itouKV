#include "itoukv/store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr std::string_view note_prefix = "note/";
constexpr std::string_view tag_prefix = "tag/";
constexpr std::string_view archive_header = "ITOUKV_NOTES_V1";
constexpr std::size_t max_tags = 32;

struct Note {
  std::string id;
  std::string title;
  std::string body;
  std::vector<std::string> tags;
};

void usage() {
  std::cout << "Usage: itoukv_notes <directory> <command> [arguments]\n"
               "  add <id> <title> <file|-> [tag ...]\n"
               "  edit <id> <title> <file|-> [tag ...]  (replaces tags)\n"
               "  show <id>\n"
               "  list [tag]\n"
               "  delete <id>\n"
               "  export <file>\n"
               "  import <file>\n"
               "  compact\n"
               "IDs and tags: 1..80 ASCII letters, digits, _ or -. '-' as input reads stdin.\n";
}

void check_name(std::string_view text, std::string_view kind) {
  if (text.empty() || text.size() > 80 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
      }))
    throw itoukv::Error(std::string(kind) + " must be 1..80 ASCII letters, digits, _ or -");
}

void check_title(std::string_view title) {
  if (title.empty() || title.size() > 4096 ||
      std::any_of(title.begin(), title.end(), [](unsigned char c) { return c < 32 || c == 127; }))
    throw itoukv::Error("title must be 1..4096 bytes without control characters");
}

void normalize_tags(std::vector<std::string>& tags, bool reject_duplicates = false) {
  if (tags.size() > max_tags) throw itoukv::Error("too many tags (max 32)");
  for (const auto& tag : tags) check_name(tag, "tag");
  std::sort(tags.begin(), tags.end());
  const auto unique_end = std::unique(tags.begin(), tags.end());
  if (reject_duplicates && unique_end != tags.end()) throw itoukv::Error("duplicate tag");
  tags.erase(unique_end, tags.end());
}

std::string note_key(std::string_view id) { return std::string(note_prefix) + std::string(id); }
std::string tag_key(std::string_view tag, std::string_view id) {
  return std::string(tag_prefix) + std::string(tag) + "/" + std::string(id);
}

void append_u32(std::string& data, std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) throw itoukv::Error("field too large");
  for (int i = 0; i < 4; ++i) data.push_back(static_cast<char>((value >> (i * 8)) & 255));
}

std::uint32_t take_u32(std::string_view data, std::size_t& cursor) {
  if (data.size() - cursor < 4) throw itoukv::Error("truncated note record");
  std::uint32_t result = 0;
  for (int i = 0; i < 4; ++i)
    result |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[cursor++])) << (i * 8);
  return result;
}

std::string take_string(std::string_view data, std::size_t& cursor, std::size_t length) {
  if (length > data.size() - cursor) throw itoukv::Error("truncated note record");
  std::string result(data.substr(cursor, length));
  cursor += length;
  return result;
}

std::string encode(const Note& note) {
  check_name(note.id, "id");
  check_title(note.title);
  std::string result = "NOTE1";
  append_u32(result, note.title.size());
  append_u32(result, note.body.size());
  append_u32(result, note.tags.size());
  for (const auto& tag : note.tags) {
    append_u32(result, tag.size());
    result += tag;
  }
  result += note.title;
  result += note.body;
  if (result.size() > itoukv::Store::max_value_bytes) throw itoukv::Error("note exceeds 16 MiB value limit");
  return result;
}

Note decode(std::string id, std::string_view value) {
  if (!value.starts_with("NOTE1")) throw itoukv::Error("invalid note record");
  std::size_t cursor = 5;
  const auto title_size = take_u32(value, cursor);
  const auto body_size = take_u32(value, cursor);
  const auto tag_count = take_u32(value, cursor);
  if (tag_count > max_tags) throw itoukv::Error("invalid note tag count");
  Note note{std::move(id), {}, {}, {}};
  for (std::uint32_t i = 0; i < tag_count; ++i) {
    const auto length = take_u32(value, cursor);
    note.tags.push_back(take_string(value, cursor, length));
  }
  note.title = take_string(value, cursor, title_size);
  note.body = take_string(value, cursor, body_size);
  if (cursor != value.size()) throw itoukv::Error("invalid note record length");
  check_title(note.title);
  normalize_tags(note.tags, true);
  return note;
}

std::string read_body(std::istream& input) {
  std::string body;
  std::array<char, 8192> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > itoukv::Store::max_value_bytes - body.size())
      throw itoukv::Error("note exceeds 16 MiB value limit");
    body.append(buffer.data(), count);
  }
  if (input.bad() || (!input.eof() && input.fail())) throw itoukv::Error("cannot read note body");
  return body;
}

std::string read_body_file(const std::string& path) {
  if (path == "-") return read_body(std::cin);
  std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
  if (!input) throw itoukv::Error("cannot open note body: " + path);
  return read_body(input);
}

template <typename Action>
void for_prefix(const itoukv::Store& store, std::string prefix, Action action) {
  auto start = prefix;
  while (true) {
    const auto page = store.scan(start, 1);
    if (page.empty() || !page[0].first.starts_with(prefix)) break;
    action(page[0].first, page[0].second);
    start = page[0].first + '\0'; // Strict successor for a complete key, including prefix keys.
  }
}

std::string hex(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    result.push_back(digits[c >> 4]);
    result.push_back(digits[c & 15]);
  }
  return result;
}

std::string unhex(std::string_view text) {
  if (text.size() % 2 != 0 || text.size() > itoukv::Store::max_value_bytes * 2)
    throw itoukv::Error("invalid archive hex length");
  const auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    throw itoukv::Error("invalid archive hex digit");
  };
  std::string result;
  result.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(static_cast<char>((nibble(text[i]) << 4) | nibble(text[i + 1])));
  return result;
}

Note parse_archive_line(std::string_view line) {
  const auto first = line.find('\t');
  const auto second = first == line.npos ? line.npos : line.find('\t', first + 1);
  const auto third = second == line.npos ? line.npos : line.find('\t', second + 1);
  if (third == line.npos || line.find('\t', third + 1) != line.npos)
    throw itoukv::Error("expected id<TAB>title_hex<TAB>body_hex<TAB>tags");
  Note note{std::string(line.substr(0, first)), unhex(line.substr(first + 1, second - first - 1)),
            unhex(line.substr(second + 1, third - second - 1)), {}};
  const auto tags = line.substr(third + 1);
  std::size_t start = 0;
  while (start < tags.size()) {
    const auto end = tags.find(',', start);
    note.tags.emplace_back(tags.substr(start, end == tags.npos ? end : end - start));
    if (end == tags.npos) break;
    start = end + 1;
    if (start == tags.size()) throw itoukv::Error("empty tag");
  }
  normalize_tags(note.tags, true);
  encode(note); // Validate the complete on-disk record before any import writes.
  return note;
}

template <typename Action>
void read_archive(const std::string& path, Action action) {
  std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
  if (!input) throw itoukv::Error("cannot open archive: " + path);
  std::string line;
  if (!std::getline(input, line)) throw itoukv::Error("missing archive header");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line != archive_header) throw itoukv::Error("invalid archive header");
  std::size_t number = 1;
  while (std::getline(input, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    try { action(parse_archive_line(line)); }
    catch (const itoukv::Error& error) {
      throw itoukv::Error("archive line " + std::to_string(number) + ": " + error.what());
    }
  }
  if (input.bad()) throw itoukv::Error("cannot read archive");
}

void export_notes(const itoukv::Store& store, const std::string& path) {
  std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
  if (!output) throw itoukv::Error("cannot create archive: " + path);
  output << archive_header << '\n';
  for_prefix(store, std::string(note_prefix), [&](const std::string& key, const std::string& value) {
    const auto note = decode(key.substr(note_prefix.size()), value);
    output << note.id << '\t' << hex(note.title) << '\t' << hex(note.body) << '\t';
    for (std::size_t i = 0; i < note.tags.size(); ++i) {
      if (i) output << ',';
      output << note.tags[i];
    }
    output << '\n';
    if (!output) throw itoukv::Error("cannot write archive");
  });
  output.flush();
  if (!output) throw itoukv::Error("cannot write archive");
}

void import_notes(itoukv::Store& store, const std::string& path) {
  std::unordered_set<std::string> ids;
  read_archive(path, [&](const Note& note) {
    if (!ids.insert(note.id).second) throw itoukv::Error("duplicate note id: " + note.id);
    if (store.get(note_key(note.id))) throw itoukv::Error("note already exists: " + note.id);
  });
  itoukv::WriteBatch batch;
  std::size_t payload = 0;
  const auto flush = [&] {
    if (!batch.empty()) store.write(batch);
    batch.clear();
    payload = 0;
  };
  read_archive(path, [&](const Note& note) {
    const auto key = note_key(note.id);
    const auto value = encode(note);
    std::size_t needed = 9 + key.size() + value.size();
    for (const auto& tag : note.tags) needed += 9 + tag_key(tag, note.id).size();
    const auto operations = 1 + note.tags.size();
    if (batch.size() + operations > itoukv::WriteBatch::max_operations ||
        payload + needed > itoukv::WriteBatch::max_payload_bytes) flush();
    batch.put(key, value);
    for (const auto& tag : note.tags) batch.put(tag_key(tag, note.id), "");
    payload += needed;
  });
  flush();
}

int run(const std::vector<std::string>& args) {
  if (args.empty() || args[0] == "--help" || args[0] == "-h") { usage(); return 0; }
  if (args.size() < 2) { usage(); return 64; }
  const auto& command = args[1];
  const auto count = args.size() - 2;
  if (!(((command == "add" || command == "edit") && count >= 3 && count <= 3 + max_tags) ||
        ((command == "show" || command == "delete" || command == "export" || command == "import") && count == 1) ||
        (command == "list" && count <= 1) || (command == "compact" && count == 0))) {
    usage(); return 64;
  }
  try {
    if (command == "add" || command == "edit" || command == "show" || command == "delete")
      check_name(args[2], "id");
    if (command == "list" && count == 1) check_name(args[2], "tag");
    itoukv::Store store(std::filesystem::u8path(args[0]));
    if (command == "add" || command == "edit") {
      const auto& id = args[2];
      const auto old_value = store.get(note_key(id));
      if (command == "add" && old_value) throw itoukv::Error("note already exists: " + id);
      if (command == "edit" && !old_value) { std::cerr << "note not found\n"; return 2; }
      Note note{id, args[3], read_body_file(args[4]), {args.begin() + 5, args.end()}};
      normalize_tags(note.tags);
      const auto value = encode(note);
      itoukv::WriteBatch batch;
      batch.put(note_key(id), value);
      if (old_value) {
        const auto old = decode(id, *old_value);
        for (const auto& tag : old.tags)
          if (!std::binary_search(note.tags.begin(), note.tags.end(), tag)) batch.erase(tag_key(tag, id));
      }
      for (const auto& tag : note.tags) batch.put(tag_key(tag, id), "");
      store.write(batch);
      std::cout << "SAVED " << id << '\n';
    } else if (command == "show") {
      const auto value = store.get(note_key(args[2]));
      if (!value) { std::cerr << "note not found\n"; return 2; }
      const auto note = decode(args[2], *value);
      std::cout << "ID: " << note.id << "\nTitle: " << note.title << "\nTags: ";
      for (std::size_t i = 0; i < note.tags.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << note.tags[i];
      }
      std::cout << "\n\n";
      std::cout.write(note.body.data(), static_cast<std::streamsize>(note.body.size()));
    } else if (command == "list") {
      const auto prefix = count ? tag_key(args[2], "") : std::string(note_prefix);
      for_prefix(store, prefix, [&](const std::string& key, const std::string& value) {
        const auto id = key.substr(prefix.size());
        const auto stored = count ? store.get(note_key(id)) : std::optional<std::string>(value);
        if (!stored) throw itoukv::Error("orphan tag index: " + key);
        const auto note = decode(id, *stored);
        std::cout << note.id << '\t' << note.title << '\t';
        for (std::size_t i = 0; i < note.tags.size(); ++i) {
          if (i) std::cout << ',';
          std::cout << note.tags[i];
        }
        std::cout << '\n';
      });
    } else if (command == "delete") {
      const auto value = store.get(note_key(args[2]));
      if (!value) { std::cerr << "note not found\n"; return 2; }
      const auto note = decode(args[2], *value);
      itoukv::WriteBatch batch;
      batch.erase(note_key(note.id));
      for (const auto& tag : note.tags) batch.erase(tag_key(tag, note.id));
      store.write(batch);
      std::cout << "DELETED " << note.id << '\n';
    } else if (command == "export") {
      export_notes(store, args[2]);
      std::cout << "EXPORTED\n";
    } else if (command == "import") {
      import_notes(store, args[2]);
      std::cout << "IMPORTED\n";
    } else {
      store.compact();
      std::cout << "COMPACTED\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "itoukv_notes: " << error.what() << '\n';
    return 1;
  }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  _setmode(_fileno(stdin), _O_BINARY);
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
  return run(args);
}
#else
int main(int argc, char* argv[]) { return run({argv + 1, argv + argc}); }
#endif
