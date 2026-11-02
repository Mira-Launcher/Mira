#include "LibraryActions.h"

#include <QStringList>
#include <QWidget>

#include "../app/Notify.h"
#include "../client/api/Artwork.h"
#include "../client/api/Config.h"
#include "../client/api/Library.h"
#include "GamePresentation.h"

namespace mira_gui::actions {

void ScanLibrary(QWidget* parent) {
  api::ScanLibraryAsync(parent, [parent](ScanResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not scan the library.", result.error);
      return;
    }
    // New games show up in the grid on their own; only "nothing happened"
    // has no visible result of its own.
    if (result.added == 0 && result.missing == 0 && result.restored == 0) {
      notify::Notice(parent, "Scan finished. No changes.");
    }
  });
}

void SyncDesktopEntries(QWidget* parent) {
  api::SyncDesktopEntriesAsync(parent, [parent](DesktopEntrySyncResult result) {
    if (!result.ok)
      notify::FailedRequest(parent, "Could not regenerate desktop entries.", result.error);
  });
}

void RemoveAllDesktopEntries(QWidget* parent) {
  // desktop_entries.enabled is the only lever that actually makes Sync()
  // remove every mira-<id>.desktop entry rather than immediately rewriting
  // them (see desktop::DesktopEntries::Sync), so there's no "wipe once, stay
  // enabled" concept, so this is honest about turning the setting off too.
  if (!notify::Confirm(parent, "Remove all desktop entries",
                       "This turns off desktop entries and deletes every one Mira generated. "
                       "Re-enable them any time in Settings → Desktop entries.",
                       "Remove all", /*destructive=*/true)) {
    return;
  }
  const ConfigEdit edit{"desktop_entries.enabled", "a boolean", "false"};
  api::PatchConfigAsync(parent, {edit}, [parent](PatchConfigResult patch_result) {
    if (!patch_result.ok) {
      notify::FailedRequest(parent, "Could not turn off desktop entries.", patch_result.error);
      return;
    }
    api::SyncDesktopEntriesAsync(parent, [parent](DesktopEntrySyncResult sync_result) {
      if (!sync_result.ok)
        notify::FailedRequest(parent, "Could not remove the desktop entries.", sync_result.error);
    });
  });
}

void RelocateLibrary(QWidget* parent, std::function<QString(const std::string& id)> game_name) {
  if (!notify::Confirm(
          parent, "Move every game to Mira's folders",
          "Move every game's files into your games folder, and each prefix into the prefixes "
          "folder, named after the game? Games installed by a store (Steam, Epic, GOG, itch.io) "
          "keep their install folder; only the prefix moves. Games on another drive are copied "
          "then deleted, which can take a while.",
          "Move games")) {
    return;
  }
  api::RelocateLibraryAsync(parent, [parent, game_name](RelocateLibraryResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not move the games.", result.error);
      return;
    }
    // The moved games arrive as game.updated events.
    if (result.moved > 0 || result.errors.empty()) {
      notify::Notice(
          parent,
          result.moved == 0
              ? QString("Every game was already in place.")
              : QString("Moved %1 game%2.").arg(result.moved).arg(result.moved == 1 ? "" : "s"));
    }
    if (!result.errors.empty()) {
      QStringList failed;
      for (const GameFailure& failure : result.errors) {
        const QString name = game_name(failure.id);
        failed << (name.isEmpty() ? QString::fromStdString(failure.id) : name);
      }
      notify::FailedRequest(parent, QString("Could not move %1.").arg(failed.join(", ")),
                            result.errors.front().error);
    }
  });
}

void FetchMissingArtwork(QWidget* parent, std::function<void()> on_finished) {
  // mirad decides what's missing, and Activity shows it going.
  api::RefreshMissingArtworkAsync(parent, [parent, on_finished](MetadataBatchResult result) {
    on_finished();
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not fetch missing cover art.", result.error);
    } else if (result.refreshed + result.failed == 0) {
      notify::Notice(parent, "Every game already has cover art.");
    } else {
      notify::Notice(parent, BatchRefreshSummary(result));
    }
  });
}

}  // namespace mira_gui::actions
