#include "InstallActions.h"

#include <QMenu>

#include "../app/Notify.h"
#include "../client/Jobs.h"
#include "../client/api/Stores.h"

namespace mira_gui {

bool AddInstallActions(QMenu& menu, QWidget* parent, const DownloadTracker& tracker,
                       const DownloadTracker::Entry* entry) {
  if (entry == nullptr) return false;
  const QString what = entry->update ? "update" : "install";
  const std::string source = entry->source.toStdString();
  const std::string ref = entry->ref.toStdString();
  if (entry->state == DownloadTracker::State::Paused) {
    menu.addAction("Resume " + what, parent, [parent, source, ref, update = entry->update] {
      api::InstallStoreTitleAsync(parent, source, ref, update, [parent](StoreActionResult result) {
        if (!result.ok) notify::FailedRequest(parent, "Could not resume it.", result.error);
      });
    });
    menu.addAction("Cancel " + what, parent, [parent, source, ref] {
      api::DiscardPausedInstallAsync(parent, source, ref, [parent](StoreActionResult result) {
        if (!result.ok) notify::FailedRequest(parent, "Could not cancel it.", result.error);
      });
    });
    return true;
  }
  const QString job = tracker.JobFor(*entry);
  if (job.isEmpty()) return false;
  if (DownloadTracker::CanPause(*entry)) {
    menu.addAction("Pause " + what, parent, [parent, source, ref] {
      api::PauseStoreInstallAsync(parent, source, ref, [parent](StoreActionResult result) {
        if (!result.ok) notify::FailedRequest(parent, "Could not pause it.", result.error);
      });
    });
  }
  menu.addAction("Cancel " + what, parent, [parent, job = job.toStdString()] {
    jobs::Cancel(parent, job, [parent](ApiError error) {
      if (!error.message.empty()) notify::FailedRequest(parent, "Could not cancel it.", error);
    });
  });
  return true;
}

}  // namespace mira_gui
