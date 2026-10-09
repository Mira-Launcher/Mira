#include "steam/Shortcuts.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

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

  // Reads children up to kEnd into `parent`.
  bool Object(Node& parent) {
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
          if (!Object(node)) return false;
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
  std::error_code ec;
  const fs::path userdata = steam_root / "userdata";
  for (const fs::directory_entry& account : fs::directory_iterator(userdata, ec)) {
    const std::string id = account.path().filename().string();
    // "0" is Steam's placeholder before anyone signs in.
    if (id == "0" || !fs::is_directory(account.path() / "config", ec)) continue;
    const fs::path file = account.path() / "config" / "shortcuts.vdf";

    Node root{kObject, "shortcuts"};
    if (fs::exists(file, ec)) {
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
        return Err("shortcuts_unreadable", std::format("couldn't read {}", file.string()));
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
      root.children.push_back({kObject, std::to_string(root.children.size())});
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
    const fs::path temp = fs::path(file).concat(".mira-tmp");
    {
      std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
      stream.write(out.data(), std::streamsize(out.size()));
      if (!stream) return Err("io_error", std::format("couldn't write {}", temp.string()));
    }
    fs::rename(temp, file, ec);
    if (ec) return Err("io_error", std::format("couldn't replace {}: {}", file.string(), ec.message()));
    (adding ? change.added : change.updated).push_back(id);
  }
  return change;
}

}  // namespace mira::steam
