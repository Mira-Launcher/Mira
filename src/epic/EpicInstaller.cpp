#include "epic/EpicInstaller.h"

#include "epic/EpicImporter.h"
#include "epic/Legendary.h"
#include "library/StoreProgress.h"

namespace mira::epic {

Result<void> InstallTitle(config::Config& config, store::GameStore& games, api::EventBus& events,
                          const std::string& verb, const std::string& app_name) {
  // A leading '-' would be read as an option.
  if (app_name.empty() || app_name.starts_with('-'))
    return Err("invalid_ref", "that isn't an Epic app name");
  library::StoreProgress progress(events, "epic", app_name);
  std::vector<std::string> args = {verb, app_name, "-y"};
  // Without it legendary installs into its own default, ~/Games. An update stays where the game is.
  if (verb == "install")
    args.insert(args.end(), {"--base-path", config.GetPath("epic.install_root").string()});
  if (auto output =
          RunLegendary(config, args, [&progress](std::string_view chunk) { progress.Feed(chunk); });
      !output) {
    return std::unexpected(output.error());
  }
  // Picks up the new/updated install and provisions a Wine/Proton prefix
  // for it. Also re-syncs the whole catalog as a side effect; harmless, and
  // not worth a narrower "import just this one title" path for what's
  // already a slow, human-triggered operation.
  EpicImporter importer(config, games, events);
  if (auto imported = importer.Import(); !imported) return std::unexpected(imported.error());
  return {};
}

}  // namespace mira::epic
