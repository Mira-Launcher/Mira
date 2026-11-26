#include "gog/GogInstaller.h"

#include "core/StoreErrors.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "library/StoreProgress.h"

namespace mira::gog {
Result<void> InstallTitle(config::Config& config, store::GameStore& games, api::EventBus& events,
                          const std::string& id) {
  if (auto ready = CheckReady(config); !ready) return ready;

  // gogdl creates <root>/<Title>/ itself. An update reuses wherever the
  // game already is, so an old <id>/<Title>/ install isn't duplicated.
  std::filesystem::path root = config.GetPath("gog.install_root");
  if (const std::filesystem::path existing = FindGameDir(config, id); !existing.empty()) {
    root = existing.parent_path();
  } else if (config.GetString("gog.folder_naming") == "id") {
    root /= id;
  }
  library::StoreProgress progress(events, "gog", id);
  const char* dlcs = config.GetBool("gog.install_dlc") ? "--with-dlcs" : "--skip-dlcs";
  if (auto output =
          RunGogdl(config, {"download", id, "--path", root.string(), "--platform", "windows", dlcs},
                   [&progress](std::string_view chunk) { progress.Feed(chunk); });
      !output) {
    return std::unexpected(output.error());
  }
  const std::filesystem::path install_dir = FindGameDir(config, id);
  if (install_dir.empty())
    return Err("install_dir_missing", "gogdl finished but no goggame-" + id + ".info was found");

  GogImporter importer(config, games, events);
  if (auto imported = importer.ImportPath(id, install_dir); !imported)
    return std::unexpected(imported.error());
  return {};
}

}  // namespace mira::gog
