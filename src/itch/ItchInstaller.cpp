#include "itch/ItchInstaller.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <json.hpp>

#include "core/Json.h"
#include "core/StoreErrors.h"
#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"

namespace mira::itch {
using nlohmann::json;

Result<void> InstallTitle(config::Config& config, store::GameStore& games, api::EventBus& events,
                          const std::string& game_id) {
  if (auto ready = CheckReady(config); !ready) return ready;
  std::int64_t numeric_id = 0;
  const auto [end, parsed] =
      std::from_chars(game_id.data(), game_id.data() + game_id.size(), numeric_id);
  if (parsed != std::errc() || end != game_id.data() + game_id.size() || numeric_id <= 0) {
    return Err("invalid_ref", "an itch.io game id is a number");
  }

  const Result<std::int64_t> profile_id = CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());
  if (auto location = EnsureInstallLocation(config); !location)
    return std::unexpected(location.error());

  // Per butlerd's spec, Install.Queue
  // picks an upload and returns {id, stagingFolder}; Install.Perform
  // fetches it using exactly those two values back.
  const Result<json> queued = Call(
      config, "Install.Queue",
      {{"game", {{"id", numeric_id}}}, {"profileId", *profile_id}, {"installLocationId", "mira"}});
  if (!queued) return std::unexpected(queued.error());

  const std::string install_id = queued->value("id", std::string());
  const std::string staging_folder = queued->value("stagingFolder", std::string());
  if (install_id.empty() || staging_folder.empty()) {
    return Err("itch_queue_failed", "Install.Queue didn't return an id/stagingFolder");
  }

  // Progress arrives as notifications; passed on about once a second.
  auto last_sent = std::chrono::steady_clock::time_point{};
  const auto on_notification = [&](const std::string& method, const json& params) {
    if (method != "Progress") return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_sent < std::chrono::seconds(1)) return;
    last_sent = now;
    events.Publish("library.install.progress", {{"source", "itch"},
                                                {"ref", game_id},
                                                {"progress", core::JsonDouble(params, "progress")},
                                                {"eta", core::JsonDouble(params, "eta")},
                                                {"bps", core::JsonDouble(params, "bps")}});
  };
  if (auto performed =
          CallLong(config, "Install.Perform",
                   {{"id", install_id}, {"stagingFolder", staging_folder}}, on_notification);
      !performed) {
    return std::unexpected(performed.error());
  }

  ItchImporter importer(config, games, events);
  if (auto imported = importer.Import(); !imported) return std::unexpected(imported.error());
  return {};
}

}  // namespace mira::itch
