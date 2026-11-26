#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mira::config {
class Config;
}
namespace mira::store {
class GameStore;
}

namespace mira::library {

// A mounted filesystem from /proc/self/mounts.
struct Mount {
  std::string device;  // /dev/nvme1n1p1
  std::filesystem::path path;
  std::string type;
  bool read_only = false;
};

std::vector<Mount> ParseMounts(const std::string& text);

// An internal drive with room for games: a disk filesystem, writable, not USB or removable, not the
// boot drive (which has ~/Mira), with at least 20 GB free. One per device.
struct Drive {
  std::string id;  // its filesystem UUID, else the device
  std::filesystem::path mount;
};
std::vector<Drive> GameDrives();

// Gives every drive from GameDrives a Mira folder (<drive>/Mira with Games and Applications) once,
// with library.auto_drives on. A drive whose folder the user removed isn't set up again.
void SetUpDrives(config::Config& config, store::GameStore& games);

}  // namespace mira::library
