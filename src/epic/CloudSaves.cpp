#include "epic/CloudSaves.h"

#include <chrono>
#include <format>
#include <regex>

#include "core/Json.h"
#include "epic/Legendary.h"

namespace mira::epic {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

std::unexpected<Error> Failed(const model::Game& game, std::string_view doing) {
  return Err("cloud_sync_failed",
             std::format("Couldn't {} {}'s saves with Epic Games's cloud.", doing, game.name),
             kConnectionHint);
}

class EpicCloud final : public saves::CloudSide {
 public:
  std::string_view Name() const override { return "Epic Games"; }

  Result<saves::CloudInfo> Info(const config::Config& config,
                                const model::Game& game) const override {
    if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
    const auto info = RunLegendaryJson(config, {"info", game.source_ref});
    if (!info) return std::unexpected(info.error());
    const json about = core::JsonObject(*info, "game");
    const std::string folder = core::JsonString(about, "cloud_save_folder");
    if (!core::JsonBool(about, "cloud_saves_supported") || folder.empty())
      return saves::CloudInfo{.reason = "Epic Games doesn't keep this game's saves in its cloud."};
    return saves::CloudInfo{.supported = true, .locations = {{"saves", folder}}};
  }

  // Epic lists a game's cloud saves as one manifest per upload, named by its UTC time.
  Result<saves::CloudListing> List(const config::Config& config, const model::Game& game,
                                   const saves::CloudInfo&,
                                   const saves::CloudLocation&) const override {
    if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
    const auto output = RunLegendary(config, {"list-saves", game.source_ref});
    if (!output) return Failed(game, "list");
    static const std::regex manifest(
        R"((\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2})\.manifest)");
    saves::CloudListing listing;
    for (auto it = std::sregex_iterator(output->begin(), output->end(), manifest);
         it != std::sregex_iterator(); ++it) {
      const auto number = [&](int group) { return std::stoi((*it)[group].str()); };
      const std::chrono::year_month_day day{
          std::chrono::year(number(1)), std::chrono::month(number(2)), std::chrono::day(number(3))};
      const auto time = std::chrono::sys_days(day) + std::chrono::hours(number(4)) +
                        std::chrono::minutes(number(5)) + std::chrono::seconds(number(6));
      ++listing.count;
      listing.newest_ns = std::max<std::int64_t>(
          listing.newest_ns,
          std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
    }
    return listing;
  }

  Result<void> Move(const config::Config& config, const model::Game& game, const saves::CloudInfo&,
                    const saves::CloudLocation&, const fs::path& folder,
                    saves::Direction direction) const override {
    if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
    const bool down = direction == saves::Direction::Download;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const auto output =
        RunLegendary(config, {"sync-saves", game.source_ref, "--save-path", folder.string(),
                              down ? "--force-download" : "--force-upload", "-y"});
    // A save path it couldn't record ends the sync early with exit 0.
    if (!output || output->find("Unable to lock") != std::string::npos)
      return Failed(game, down ? "download" : "upload");
    return {};
  }
};

}  // namespace

const saves::CloudSide& Cloud() {
  static const EpicCloud cloud;
  return cloud;
}

}  // namespace mira::epic
