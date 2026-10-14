#include "steam/Shortcuts.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

#include "core/AtomicFile.h"

namespace mira::steam {
namespace {

// Valve's binary KeyValues, as shortcuts.vdf uses it: a type byte, a
// NUL-terminated name, then the value. Objects end with kEnd.
enum Type : std::uint8_t { kObject = 0, kString = 1, kInt = 2, kFloat = 3, kUint64 = 7, kEnd = 8 };

struct Node {
  Type type = kObject;
  std::string name;
  std::string bytes;  // a string's text, or a number's raw little-endian bytes
  std::vector<Node> children;

  Node() = default;
  Node(Type t, std::string n = {}, std::string b = {}) : type(t), name(std::move(n)), bytes(std::move(b)) {}

  Node* Child(std::string_view key) {
    for (Node& child : children) {
      if (child.name.size() == key.size() &&
          std::equal(key.begin(), key.end(), child.name.begin(),
                     [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
        return &child;
      }
    }
    return nullptr;
  }
};

struct Reader {
  std::string_view data;
  size_t at = 0;

  std::optional<std::string> CString() {
    const size_t end = data.find('\0', at);
    if (end == std::string_view::npos) return std::nullopt;
    std::string text(data.substr(at, end - at));
    at = end + 1;
    return text;
  }

  // Reads children up to kEnd into `parent`, refusing nesting deeper than Steam writes.
  bool Object(Node& parent, int depth = 0) {
    if (depth > 64) return false;
    while (at < data.size()) {
      const auto type = static_cast<Type>(data[at++]);
      if (type == kEnd) return true;
      Node node{type};
      auto name = CString();
      if (!name) return false;
      node.name = std::move(*name);
      size_t width = 0;
      switch (type) {
        case kObject:
          if (!Object(node, depth + 1)) return false;
          break;
        case kString: {
          auto text = CString();
          if (!text) return false;
          node.bytes = std::move(*text);
          break;
        }
        case kInt:
        case kFloat: width = 4; break;
        case kUint64: width = 8; break;
        default: return false;
      }
      if (width > 0) {
        if (at + width > data.size()) return false;
        node.bytes = std::string(data.substr(at, width));
        at += width;
      }
      parent.children.push_back(std::move(node));
    }
    return false;
  }
};

void Write(std::string& out, const Node& node) {
  out += char(node.type);
  out += node.name;
  out += '\0';
  if (node.type == kObject) {
    for (const Node& child : node.children) Write(out, child);
    out += char(kEnd);
  } else {
    out += node.bytes;
    if (node.type == kString) out += '\0';
  }
}

Node Str(std::string name, std::string text) { return {kString, std::move(name), std::move(text)}; }

Node Int(std::string name, std::uint32_t value) {
  std::string bytes(4, '\0');
  for (int i = 0; i < 4; ++i) bytes[size_t(i)] = char((value >> (8 * i)) & 0xff);
  return {kInt, std::move(name), std::move(bytes)};
}

std::uint32_t Crc32(std::string_view text) {
  std::uint32_t crc = 0xffffffffu;
  for (const char c : text) {
    crc ^= std::uint8_t(c);
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::string Quoted(const std::string& path) { return "\"" + path + "\""; }

// Sets `key` to `want`, adding it if missing; true when that changed anything.
bool Set(Node& entry, Node want) {
  if (Node* have = entry.Child(want.name)) {
    if (have->type == want.type && have->bytes == want.bytes) return false;
    *have = std::move(want);
  } else {
    entry.children.push_back(std::move(want));
  }
  return true;
}

}  // namespace

Result<ShortcutChange> EnsureShortcut(const std::filesystem::path& steam_root, const std::string& name,
                                      const std::string& exe, const std::string& launch_options) {
  namespace fs = std::filesystem;
  ShortcutChange change;
  std::optional<Error> failure;
  std::size_t accounts = 0;
  std::error_code ec;
  const fs::path userdata = steam_root / "userdata";
  for (const fs::directory_entry& account : fs::directory_iterator(userdata, ec)) {
    const std::string id = account.path().filename().string();
    // "0" is Steam's placeholder before anyone signs in.
    if (id == "0" || !fs::is_directory(account.path() / "config", ec)) continue;
    ++accounts;
    const fs::path file = account.path() / "config" / "shortcuts.vdf";

    Node root{kObject, "shortcuts"};
    // An empty file holds no shortcuts, so it's rewritten like a missing one.
    if (fs::exists(file, ec) && fs::file_size(file, ec) != 0) {
      std::ifstream in(file, std::ios::binary);
      std::stringstream buffer;
      buffer << in.rdbuf();
      const std::string data = buffer.str();
      Reader reader{data};
      Node top;
      Node* parsed = nullptr;
      if (reader.Object(top) && top.children.size() == 1 && top.children[0].type == kObject) {
        parsed = &top.children[0];
      }
      // A file this can't read is left alone rather than rewritten without the user's shortcuts.
      if (parsed == nullptr) {
        failure = Err("shortcuts_unreadable", std::format("couldn't read {}", file.string())).error();
        change.failed.push_back(id);
        continue;
      }
      root = std::move(*parsed);
    }

    Node* entry = nullptr;
    for (Node& child : root.children) {
      Node* app_name = child.Child("AppName");
      if (child.type == kObject && app_name != nullptr && app_name->bytes == name) entry = &child;
    }
    const bool adding = entry == nullptr;
    if (adding) {
      // After the highest index, not at the count: another tool may have left gaps.
      std::int64_t next = 0;
      for (const Node& child : root.children) {
        std::int64_t index = -1;
        const auto [end, ec] = std::from_chars(child.name.data(), child.name.data() + child.name.size(), index);
        if (ec == std::errc() && end == child.name.data() + child.name.size()) next = std::max(next, index + 1);
      }
      root.children.push_back({kObject, std::to_string(next)});
      entry = &root.children.back();
    }
    bool changed = adding;
    changed |= Set(*entry, Int("appid", Crc32(Quoted(exe) + name) | 0x80000000u));
    changed |= Set(*entry, Str("AppName", name));
    changed |= Set(*entry, Str("Exe", Quoted(exe)));
    changed |= Set(*entry, Str("StartDir", Quoted(fs::path(exe).parent_path().string())));
    changed |= Set(*entry, Str("LaunchOptions", launch_options));
    if (adding) {
      for (Node node : {Str("icon", ""), Str("ShortcutPath", ""), Int("IsHidden", 0), Int("AllowDesktopConfig", 1),
                        Int("AllowOverlay", 1), Int("OpenVR", 0), Int("Devkit", 0), Str("DevkitGameID", ""),
                        Int("DevkitOverrideAppID", 0), Int("LastPlayTime", 0), Str("FlatpakAppID", "")}) {
        entry->children.push_back(std::move(node));
      }
      entry->children.push_back({kObject, "tags"});
    }
    if (!changed) continue;

    std::string out;
    Write(out, root);
    out += char(kEnd);
    if (auto ok = WriteFileAtomic(file, out, "io_error", true); !ok) {
      failure = ok.error();
      change.failed.push_back(id);
      continue;
    }
    (adding ? change.added : change.updated).push_back(id);
  }
  if (failure && change.failed.size() == accounts) return std::unexpected(*failure);
  return change;
}

}  // namespace mira::steam
