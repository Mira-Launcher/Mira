#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Exec.h"

namespace mira::runner {

// Where a store's command-line tool was found.
struct ToolStatus {
  bool installed = false;
  std::string source = "none";  // "override" | "managed" | "path" | "none"
  std::string path;
  std::string version;
};

nlohmann::json ToJson(const ToolStatus& status);

// A store's tool, and whether its account is signed in.
struct AuthStatus {
  ToolStatus tool;
  bool authenticated = false;
  std::string account;  // when the tool reports one
};

// One store's command-line tool (legendary, gogdl, nile, butler, humble-cli).
struct StoreTool {
  const char* store;         // "gog": the store's id in settings, errors and events
  const char* store_name;    // "GOG"
  const char* binary;        // "gogdl": the executable's name on $PATH, and in messages
  const char* override_key;  // "gog.gogdl_bin": the setting that points at a copy of the tool
  std::filesystem::path (*managed_path)(const config::Config& config);  // where Mira's own download lives
};

// The override setting first, then Mira's own download, then $PATH. Doesn't need the tool to work.
ToolStatus DetectTool(const config::Config& config, const StoreTool& tool);

// Ok when the tool is installed and the account signed in, else the error that tells the user what to fix.
Result<void> CheckStoreReady(const StoreTool& tool, const AuthStatus& status);

// Runs the tool with `leading` then `args` and returns its combined output: Err("<binary>_missing") when it
// isn't there and Err("<binary>_failed") on a nonzero exit.
Result<std::string> RunTool(const config::Config& config, const StoreTool& tool, const std::vector<std::string>& args,
                            const OutputFn& on_output = {}, const std::vector<std::string>& leading = {});

}  // namespace mira::runner
