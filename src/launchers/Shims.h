#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

// mira-winapp-shims: DLLs that stand in for Wine's where Windows apps need more than it has.
namespace mira::launchers::shims {

// A shim replacing Wine's `name`.dll; it forwards to Wine's own copy, kept as `wine_copy`.dll when
// not empty.
struct Shim {
  std::string_view name;
  std::string_view wine_copy;
};

// The folder holding the shim DLLs: launchers.shims_dir if set, else the release from
// launchers.shims_url, downloaded again when a newer one is out. `version` is the release the
// folder holds (empty for shims_dir).
Result<std::filesystem::path> Dir(const config::Config& config,
                                  const std::filesystem::path& downloads,
                                  std::string* version = nullptr);

// Copies `list` into the prefix's system32. Each needs a "native,builtin" DLL override to load.
Result<void> Install(const config::Config& config, const runner::RunnerRegistry& runners,
                     const model::Game& host, const std::filesystem::path& downloads,
                     std::span<const Shim> list, std::string* version = nullptr);

// Sets each of `list` to load native first, in the prefix's registry (which Wine's services read
// too).
Result<void> Override(const runner::RunnerRegistry& runners, const model::Game& host,
                      std::span<const Shim> list);

// The file's first line.
std::string ReadLine(const std::filesystem::path& file);

}  // namespace mira::launchers::shims
