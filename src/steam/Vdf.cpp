#include "steam/Vdf.h"

#include "core/Strings.h"

namespace mira::steam {
namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  // depth 0 is the file itself; deeper nesting than Steam writes would only overflow the stack.
  Result<VdfValue> ParseObjectBody(int depth) {
    if (depth > 64) return Err("vdf_too_deep", "objects nested too deep");
    const bool top_level = depth == 0;
    VdfValue node;
    while (true) {
      SkipTrivia();
      if (pos_ >= text_.size()) {
        if (!top_level) return Err("vdf_unterminated", "object never closed");
        return node;
      }
      if (text_[pos_] == '}') {
        if (top_level) return Err("vdf_unexpected_brace", "unmatched '}'");
        ++pos_;
        return node;
      }

      auto key = ParseString();
      if (!key) return std::unexpected(key.error());
      SkipTrivia();
      if (pos_ < text_.size() && text_[pos_] == '{') {
        ++pos_;
        auto child = ParseObjectBody(depth + 1);
        if (!child) return std::unexpected(child.error());
        node.children[strings::ToLower(*key)] = std::move(*child);
      } else {
        auto value = ParseString();
        if (!value) return std::unexpected(value.error());
        VdfValue leaf;
        leaf.scalar = std::move(*value);
        node.children[strings::ToLower(*key)] = std::move(leaf);
      }
    }
  }

 private:
  void SkipTrivia() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }

  Result<std::string> ParseString() {
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      return Err("vdf_expected_string", "expected a quoted string");
    }
    ++pos_;
    std::string value;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) ++pos_;
      value += text_[pos_++];
    }
    if (pos_ >= text_.size()) return Err("vdf_unterminated_string", "string never closed");
    ++pos_;  // closing quote
    return value;
  }

  std::string_view text_;
  size_t pos_ = 0;
};

}  // namespace

const VdfValue* VdfValue::Get(std::initializer_list<std::string_view> path) const {
  const VdfValue* node = this;
  for (const std::string_view segment : path) {
    if (!node->IsObject()) return nullptr;
    const auto it = node->children.find(strings::ToLower(segment));
    if (it == node->children.end()) return nullptr;
    node = &it->second;
  }
  return node;
}

Result<VdfValue> ParseVdf(std::string_view text) {
  Parser parser(text);
  return parser.ParseObjectBody(0);
}

}  // namespace mira::steam
