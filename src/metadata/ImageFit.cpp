#include "metadata/ImageFit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

namespace mira::metadata {
namespace {

constexpr int kJpegQuality = 90;

void Append(void* context, void* data, int size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

bool IsJpeg(const std::filesystem::path& file) {
  std::unique_ptr<FILE, decltype(&std::fclose)> in(std::fopen(file.c_str(), "rb"), &std::fclose);
  unsigned char magic[3] = {};
  return in && std::fread(magic, 1, 3, in.get()) == 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
}

}  // namespace

Box SlotBox(std::string_view slot, bool title) {
  if (slot == "cover") return title ? Box{450, 675} : Box{900, 1350};
  if (slot == "hero") return {1920, 1920};
  if (slot == "logo") return {800, 800};
  if (slot == "icon") return {256, 256};
  return {1920, 1920};
}

std::optional<Fitted> FitImage(const std::filesystem::path& file, Box box) {
  int width = 0, height = 0, channels = 0;
  if (!stbi_info(file.c_str(), &width, &height, &channels)) return std::nullopt;
  const bool fits = width <= box.width && height <= box.height;
  // A JPEG that fits is kept: encoding it again only loses detail.
  if (fits && IsJpeg(file)) return std::nullopt;

  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(file.c_str(), &width, &height, &channels, 4),
                                                              &stbi_image_free);
  if (!pixels) return std::nullopt;
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  bool transparent = false;
  for (std::size_t i = 3; i < count * 4 && !transparent; i += 4) transparent = pixels.get()[i] != 255;
  if (fits && transparent) return std::nullopt;  // a PNG already, and lossless

  const double scale = std::min({1.0, static_cast<double>(box.width) / width, static_cast<double>(box.height) / height});
  const int out_width = std::max(1, static_cast<int>(std::lround(width * scale)));
  const int out_height = std::max(1, static_cast<int>(std::lround(height * scale)));
  std::vector<stbi_uc> scaled;
  const stbi_uc* out = pixels.get();
  if (out_width != width || out_height != height) {
    scaled.resize(static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 4);
    if (!stbir_resize_uint8_srgb(pixels.get(), width, height, 0, scaled.data(), out_width, out_height, 0, STBIR_RGBA)) {
      return std::nullopt;
    }
    out = scaled.data();
  }

  Fitted fitted;
  fitted.ext = transparent ? ".png" : ".jpg";
  const int written = transparent
                          ? stbi_write_png_to_func(Append, &fitted.bytes, out_width, out_height, 4, out, 0)
                          : stbi_write_jpg_to_func(Append, &fitted.bytes, out_width, out_height, 4, out, kJpegQuality);
  if (written == 0 || fitted.bytes.empty()) return std::nullopt;
  return fitted;
}

}  // namespace mira::metadata
