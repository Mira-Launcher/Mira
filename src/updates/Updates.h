#pragma once

#include <string>
#include <vector>

#include "config/Config.h"

// Newer releases of Mira and of what it downloaded: emulators, store tools, umu and runner builds.
namespace mira::updates {

struct Update {
  std::string kind;       // "mira", "emulator", "tool" or "runner"
  std::string id;         // the emulator's id, the store's ("gog"), "umu", or the runner's reference
  std::string name;       // "DuckStation"
  std::string installed;  // the tag or version there now
  std::string latest;
  std::string url;  // Mira's release page
};

// Asks each source at once; one that can't be reached is left out.
std::vector<Update> FindUpdates(config::Config& config);

}  // namespace mira::updates
