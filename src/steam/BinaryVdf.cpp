#include "steam/BinaryVdf.h"

#include <algorithm>
#include <cctype>

namespace mira::steam {

Node* Node::Child(std::string_view key) {
  for (Node& child : children) {
    if (child.name.size() == key.size() && std::equal(key.begin(), key.end(), child.name.begin(),
                                                      [](unsigned char a, unsigned char b) {
                                                        return std::tolower(a) == std::tolower(b);
                                                      })) {
      return &child;
    }
  }
  return nullptr;
}

std::int32_t Node::Int32() const {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(ReadLittle(bytes, 0, 4).value_or(0)));
}

std::optional<std::uint64_t> ReadLittle(std::string_view data, size_t at, size_t width) {
  if (at > data.size() || data.size() - at < width) return std::nullopt;
  std::uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value |= std::uint64_t(std::uint8_t(data[at + i])) << (8 * i);
  }
  return value;
}

std::optional<std::string> Reader::CString() {
  const size_t end = data.find('\0', at);
  if (end == std::string_view::npos) return std::nullopt;
  std::string text(data.substr(at, end - at));
  at = end + 1;
  return text;
}

std::optional<std::string> Reader::Key() {
  if (keys == nullptr) return CString();
  const auto index = ReadLittle(data, at, 4);
  if (!index) return std::nullopt;
  at += 4;
  if (*index >= keys->size()) return std::nullopt;
  return (*keys)[*index];
}

bool Reader::Object(Node& parent, int depth) {
  if (depth > 64) return false;
  while (at < data.size()) {
    const auto type = static_cast<Type>(data[at++]);
    if (type == kEnd) return true;
    if (!Item(parent, type, depth)) return false;
  }
  return false;
}

bool Reader::Item(Node& parent, Type type, int depth) {
  Node node{type};
  auto name = Key();
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
    case kFloat:
      width = 4;
      break;
    case kUint64:
      width = 8;
      break;
    default:
      return false;
  }
  if (width > 0) {
    if (at + width > data.size()) return false;
    node.bytes = std::string(data.substr(at, width));
    at += width;
  }
  parent.children.push_back(std::move(node));
  return true;
}

std::optional<Node> Reader::Single() {
  if (at >= data.size()) return std::nullopt;
  Node holder;
  const auto type = static_cast<Type>(data[at++]);
  // One object, then the end of the block around it.
  if (type != kObject || !Item(holder, type, 0) || holder.children.size() != 1 ||
      at >= data.size() || static_cast<Type>(data[at++]) != kEnd) {
    return std::nullopt;
  }
  return std::move(holder.children[0]);
}

}  // namespace mira::steam
