#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira::steam {

// Valve's binary KeyValues: a type byte, a name, then the value. Objects end with kEnd.
enum Type : std::uint8_t { kObject = 0, kString = 1, kInt = 2, kFloat = 3, kUint64 = 7, kEnd = 8 };

struct Node {
  Type type = kObject;
  std::string name;
  std::string bytes;  // a string's text, or a number's raw little-endian bytes
  std::vector<Node> children;

  Node() = default;
  Node(Type t, std::string n = {}, std::string b = {})
      : type(t), name(std::move(n)), bytes(std::move(b)) {}

  // Case-insensitive child lookup.
  Node* Child(std::string_view key);
  std::int32_t Int32() const;  // 0 when the bytes are short
};

// Little-endian integer of `width` bytes at `at`; nullopt when it doesn't fit.
std::optional<std::uint64_t> ReadLittle(std::string_view data, size_t at, size_t width);

struct Reader {
  std::string_view data;
  size_t at = 0;
  // When set, a key is a little-endian u32 index into this table instead of a string.
  const std::vector<std::string>* keys = nullptr;

  std::optional<std::string> CString();
  std::optional<std::string> Key();
  // Reads children up to kEnd into `parent`, refusing nesting deeper than Steam writes.
  bool Object(Node& parent, int depth = 0);
  // Reads one entry's name and value and appends it to `parent`.
  bool Item(Node& parent, Type type, int depth);
  // Reads a block holding one object, through the block's closing kEnd.
  std::optional<Node> Single();
};

}  // namespace mira::steam
