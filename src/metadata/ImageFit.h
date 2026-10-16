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

// metadata.art_size, resolved: kCompact about half of kHigh, kOriginal no limit but for icons.
enum class ArtSize { kCompact, kHigh, kOriginal };

// The box a slot's art is kept within. A store title not installed gets a smaller cover.
Box SlotBox(std::string_view slot, bool title = false, ArtSize size = ArtSize::kHigh);

struct Fitted {
  std::string bytes;
  std::string ext;  // ".jpg" or ".png"
};

// `file` scaled down to fit `box` and encoded as JPEG, or PNG when it has transparency. nullopt
// for a JPEG or transparent PNG that already fits, and for a file stb can't read: keep it as is.
std::optional<Fitted> FitImage(const std::filesystem::path& file, Box box);

// Whether stb can read `file` as an image.
bool IsImage(const std::filesystem::path& file);

// The MIME type of an image from its first bytes, so a file's type never rests on its extension
// (a URL's, or ".jpg" for none). "application/octet-stream" for a format not listed.
std::string ImageContentType(std::string_view bytes);
// Same, for the image in `file`.
std::string FileContentType(const std::filesystem::path& file);

// Whether `file`'s header claims more pixels than any art needs, so it's never decoded or kept.
bool IsTooLarge(const std::filesystem::path& file);

}  // namespace mira::metadata
