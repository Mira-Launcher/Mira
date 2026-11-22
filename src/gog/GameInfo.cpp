#include "gog/GameInfo.h"

#include <algorithm>
#include <json.hpp>

#include "core/Files.h"
#include "core/Paths.h"

namespace mira::gog {

std::optional<GameInfo> ReadGameInfo(const std::filesystem::path& dir) {
  std::optional<GameInfo> first;
  for (const std::filesystem::path& entry : paths::ListDir(dir)) {
    const std::string file = entry.filename().string();
    if (!file.starts_with("goggame-") || !file.ends_with(".info")) continue;
    GameInfo info{.id = file.substr(8, file.size() - 8 - 5), .name = {}};
    if (info.id.empty() ||
        !std::ranges::all_of(info.id, [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    const auto text = files::ReadFile(entry);
    const nlohmann::json json = text ? nlohmann::json::parse(*text, nullptr, false) : nullptr;
    std::string root;
    if (json.is_object()) {
      if (const auto name = json.find("name"); name != json.end() && name->is_string())
        info.name = *name;
      if (const auto it = json.find("rootGameId"); it != json.end() && it->is_string()) root = *it;
    }
    if (root.empty() || root == info.id) return info;
    if (!first) first = std::move(info);
  }
  return first;
}

}  // namespace mira::gog
