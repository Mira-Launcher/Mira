#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mira::metadata {

struct Box {
  int width = 0;
  int height = 0;
};

// The box a slot's art is kept within. A store title not installed gets a smaller cover.
Box SlotBox(std::string_view slot, bool title = false);

struct Fitted {
  std::string bytes;
  std::string ext;  // ".jpg" or ".png"
};

// `file` scaled down to fit `box` and encoded as JPEG, or PNG when it has transparency. nullopt
// for a JPEG or transparent PNG that already fits, and for a file stb can't read: keep it as is.
std::optional<Fitted> FitImage(const std::filesystem::path& file, Box box);

}  // namespace mira::metadata
