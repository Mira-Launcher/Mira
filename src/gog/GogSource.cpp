#include "core/Json.h"
#include "gog/GogSource.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include <json.hpp>

#include "gog/Gog.h"
#include "gog/GogInstaller.h"
#include "runner/Exec.h"

namespace mira::gog {
namespace {
using nlohmann::json;

Result<json> GetJson(const std::string& url, const std::string& bearer_token) {
  const Result<runner::ExecResult> result = runner::RunCurlWithSecrets(
      {"-sSL", "--max-time", "10", url}, runner::CurlConfigLine("header", "Authorization: Bearer " + bearer_token));
  if (!result) return std::unexpected(result.error());
  const json parsed = json::parse(result->output, nullptr, false);
  if (parsed.is_discarded()) return Err("gog_api_error", "GOG's API didn't return valid JSON");
  return parsed;
}

}  // namespace

Result<std::vector<library::CatalogEntry>> GogSource::Catalog(const config::Config& config,
                                                              const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("gog.enabled")) return entries;

  const Result<std::string> token = AccessToken(config);
  if (!token) return std::unexpected(token.error());

  // Confirmed against heroic-gogdl's own source (gogdl/api.py): GET
  // embed.gog.com/user/data/games -> {"owned": [id, id, ...]}. gogdl has
  // no catalog subcommand of its own to ask instead (see Gog.h).
  const Result<json> owned = GetJson("https://embed.gog.com/user/data/games", *token);
  if (!owned) return std::unexpected(owned.error());
  if (!owned->contains("owned") || !(*owned)["owned"].is_array()) return entries;

  std::vector<std::string> ids;
  for (const json& id : (*owned)["owned"]) {
    if (id.is_number_integer()) ids.push_back(std::to_string(id.get<std::int64_t>()));
  }

  // Best-effort title lookup -- ids alone are still a usable catalog (see
  // CatalogEntry::title's fallback-to-ref convention every source here
  // uses), so a failure here doesn't fail the whole listing.
  // GOG's products API rejects more than 50 ids per request (HTTP 400).
  constexpr std::size_t kIdsPerRequest = 50;
  std::unordered_map<std::string, std::string> titles;
  // Owned packs (a bundle's own entry), DLC and promo items aren't games to install.
  std::unordered_set<std::string> not_games;
  for (std::size_t start = 0; start < ids.size(); start += kIdsPerRequest) {
    std::string joined;
    for (std::size_t i = start; i < std::min(ids.size(), start + kIdsPerRequest); ++i) {
      joined += (joined.empty() ? "" : ",") + ids[i];
    }
    if (const Result<json> products =
          GetJson(std::format("https://api.gog.com/products?ids={}", joined), *token);
        products && products->is_array()) {
      for (const json& product : *products) {
        const std::string id = std::to_string(core::JsonInt(product, "id"));
        titles[id] = core::JsonString(product, "title");
        const std::string type = core::JsonString(product, "game_type");
        if ((!type.empty() && type != "game") || product.value("is_installable", true) == false) not_games.insert(id);
      }
    }
  }

  for (const std::string& id : ids) {
    if (not_games.contains(id)) continue;
    library::CatalogEntry entry;
    entry.source = "gog";
    entry.ref = id;
    const auto title = titles.find(id);
    entry.title = (title != titles.end() && !title->second.empty()) ? title->second : id;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> GogSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                               const std::string& ref) {
  GogInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> GogSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                              const std::string& ref) {
  GogInstaller installer(config, games, events);
  return installer.Update(ref);
}

}  // namespace mira::gog
