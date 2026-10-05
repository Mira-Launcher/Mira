#include "runner/StoreTool.h"

#include <algorithm>
#include <format>

#include "core/StoreErrors.h"

namespace mira::runner {
namespace fs = std::filesystem;

nlohmann::json ToJson(const ToolStatus& status) {
  return {{"installed", status.installed}, {"source", status.source}, {"path", status.path},
          {"version", status.version}};
}

ToolStatus DetectTool(const config::Config& config, const StoreTool& tool) {
  const auto found = [](const char* source, const std::string& path) {
    return ToolStatus{.installed = true, .source = source, .path = path, .version = ToolVersion(path)};
  };
  const std::string override_path = config.GetString(tool.override_key);
  if (!override_path.empty() && fs::exists(override_path)) return found("override", override_path);
  const fs::path managed = tool.managed_path(config);
  if (fs::exists(managed)) return found("managed", managed.string());
  if (const auto on_path = FindOnPath(tool.binary)) return found("path", *on_path);
  return {};
}

Result<void> CheckStoreReady(const StoreTool& tool, const AuthStatus& status) {
  if (!status.tool.installed) return StoreToolMissing(tool.store, tool.store_name, tool.binary);
  if (!status.authenticated) return StoreNotSignedIn(tool.store, tool.store_name);
  return {};
}

Result<std::string> RunTool(const config::Config& config, const StoreTool& tool, const std::vector<std::string>& args,
                            const OutputFn& on_output, const std::vector<std::string>& leading) {
  const ToolStatus status = DetectTool(config, tool);
  if (!status.installed) return StoreToolMissing(tool.store, tool.store_name, tool.binary);

  Command command;
  command.argv = {status.path};
  command.argv.insert(command.argv.end(), leading.begin(), leading.end());
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  const Result<ExecResult> result = RunAndWait(command, on_output);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    std::string code = std::format("{}_failed", tool.binary);
    std::ranges::replace(code, '-', '_');
    return Err(std::move(code), std::format("{} exited {}: {}", tool.binary, result->exit_code, result->output));
  }
  return result->output;
}

}  // namespace mira::runner
