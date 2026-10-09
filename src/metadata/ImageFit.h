#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mira::metadata {

struct Box {
  int width = 0;
  int height = 0;
  int quality = 90;  // JPEG
};

// The box a slot's art is kept within. A store title not installed gets a smaller cover, and
// `compact` (metadata.art_size) about half the size.
Box SlotBox(std::string_view slot, bool title = false, bool compact = false);

struct Fitted {
  std::string bytes;
  std::string ext;  // ".jpg" or ".png"
};

// `file` scaled down to fit `box` and encoded as JPEG, or PNG when it has transparency. nullopt
// for a JPEG or transparent PNG that already fits, and for a file stb can't read: keep it as is.
std::optional<Fitted> FitImage(const std::filesystem::path& file, Box box);

// Whether stb can read `file` as an image.
bool IsImage(const std::filesystem::path& file);

}  // namespace mira::metadata
