#include "library/PeVersion.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string_view>
#include <vector>

namespace mira::library {
namespace {

constexpr std::uint32_t kRtVersion = 16;
constexpr std::size_t kMaxResources = 64 * 1024 * 1024;

std::uint32_t Read16(std::string_view data, std::size_t at) {
  if (at + 2 > data.size()) return 0;
  return static_cast<std::uint8_t>(data[at]) | static_cast<std::uint8_t>(data[at + 1]) << 8;
}

std::uint32_t Read32(std::string_view data, std::size_t at) {
  if (at + 4 > data.size()) return 0;
  return Read16(data, at) | Read16(data, at + 2) << 16;
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | cp >> 6);
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | cp >> 12);
    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | cp >> 18);
    out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// The resource section's bytes and where it starts in memory (its RVA).
struct Resources {
  std::string data;
  std::uint32_t rva = 0;
};

std::optional<Resources> ReadResources(const std::filesystem::path& exe) {
  std::ifstream in(exe, std::ios::binary);
  if (!in) return std::nullopt;
  const auto read = [&in](std::uint64_t at, std::size_t size) {
    std::string out(size, '\0');
    in.seekg(static_cast<std::streamoff>(at));
    in.read(out.data(), static_cast<std::streamsize>(size));
    out.resize(static_cast<std::size_t>(in.gcount()));
    in.clear();
    return out;
  };
  const std::string dos = read(0, 64);
  if (dos.size() < 64 || dos[0] != 'M' || dos[1] != 'Z') return std::nullopt;
  const std::uint32_t pe = Read32(dos, 0x3C);
  const std::string headers = read(pe, 4096);
  if (headers.size() < 24 || headers.compare(0, 4, std::string("PE\0\0", 4)) != 0)
    return std::nullopt;
  const std::uint32_t sections = Read16(headers, 6);
  const std::uint32_t optional_size = Read16(headers, 20);
  const std::size_t optional = 24;
  const std::uint32_t magic = Read16(headers, optional);
  // The resource table is data directory 2.
  const std::size_t directory = optional + (magic == 0x20b ? 112 : 96) + 2 * 8;
  const std::uint32_t resource_rva = Read32(headers, directory);
  if (resource_rva == 0) return std::nullopt;
  for (std::uint32_t i = 0; i < sections; ++i) {
    const std::size_t section = optional + optional_size + i * 40;
    const std::uint32_t virtual_size = Read32(headers, section + 8);
    const std::uint32_t address = Read32(headers, section + 12);
    const std::uint32_t raw_size = Read32(headers, section + 16);
    const std::uint32_t raw_at = Read32(headers, section + 20);
    if (resource_rva < address || resource_rva >= address + std::max(virtual_size, raw_size))
      continue;
    if (raw_size > kMaxResources) return std::nullopt;
    return Resources{read(raw_at, raw_size), address};
  }
  return std::nullopt;
}

// The first entry of a resource directory at `at`, or the one with `id`; its offset field.
std::optional<std::uint32_t> Entry(std::string_view data, std::size_t at,
                                   std::optional<std::uint32_t> id) {
  const std::uint32_t named = Read16(data, at + 12);
  const std::uint32_t ids = Read16(data, at + 14);
  for (std::uint32_t i = 0; i < named + ids; ++i) {
    const std::size_t entry = at + 16 + i * 8;
    if (entry + 8 > data.size()) return std::nullopt;
    if (!id || Read32(data, entry) == *id) return Read32(data, entry + 4);
  }
  return std::nullopt;
}

std::string VersionBlock(const Resources& resources) {
  const std::string_view data = resources.data;
  // Type, then name, then language: each level a directory whose entry points at the next.
  std::optional<std::uint32_t> next = Entry(data, 0, kRtVersion);
  for (int level = 0; level < 2 && next; ++level) {
    if (!(*next & 0x80000000u)) return {};
    next = Entry(data, *next & 0x7FFFFFFFu, std::nullopt);
  }
  if (!next || (*next & 0x80000000u)) return {};
  const std::uint32_t rva = Read32(data, *next);
  const std::uint32_t size = Read32(data, *next + 4);
  if (rva < resources.rva || rva - resources.rva + size > data.size()) return {};
  return std::string(data.substr(rva - resources.rva, size));
}

}  // namespace

std::string VersionString(std::string_view block, std::string_view key) {
  std::string wide;
  for (char c : key) {
    wide += c;
    wide += '\0';
  }
  wide.append(2, '\0');
  for (std::size_t at = block.find(wide); at != std::string_view::npos;
       at = block.find(wide, at + 2)) {
    // A String: wLength, wValueLength, wType, then the key, aligned to 2 within the block.
    if (at % 2 != 0 || at < 6) continue;
    std::size_t value = (at + wide.size() + 3) & ~std::size_t{3};
    std::string out;
    for (; value + 1 < block.size(); value += 2) {
      std::uint32_t unit = Read16(block, value);
      if (unit == 0) break;
      if (unit >= 0xD800 && unit < 0xDC00) {
        const std::uint32_t low = Read16(block, value + 2);
        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        value += 2;
      }
      AppendUtf8(out, unit);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  }
  return {};
}

std::string PeProductName(const std::filesystem::path& exe) {
  const std::optional<Resources> resources = ReadResources(exe);
  if (!resources) return {};
  const std::string block = VersionBlock(*resources);
  std::string name = VersionString(block, "ProductName");
  if (name.empty()) name = VersionString(block, "FileDescription");
  return name;
}

}  // namespace mira::library
