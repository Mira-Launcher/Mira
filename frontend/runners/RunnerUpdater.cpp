#include "RunnerUpdater.h"

#include <QWidget>

#include "../activity/DownloadTracker.h"
#include "../app/Notify.h"
#include "../client/api/Runners.h"

namespace mira_gui {

RunnerUpdater::RunnerUpdater(DownloadTracker* downloads, QWidget* window)
    : QObject(window), downloads_(downloads), window_(window) {
  connect(downloads_, &DownloadTracker::Changed, this, &RunnerUpdater::DownloadChanged);
}

void RunnerUpdater::Update(const Build& old, const std::string& release,
                           std::function<void(bool ok, const ApiError& error)> started) {
  replacing_[release] = old;
  api::UpdateRunnerAsync(
      this, old.reference,
      [this, release, started = std::move(started)](RunnerDownloadResult result) {
        if (!result.ok) replacing_.erase(release);
        started(result.ok, result.error);
      });
}

void RunnerUpdater::DownloadChanged(const QString& key) {
  if (!key.startsWith("runner:")) return;
  const DownloadTracker::Entry* entry = downloads_->Find(key);
  if (entry == nullptr) return;
  if (entry->state == DownloadTracker::State::Failed) replacing_.erase(entry->ref.toStdString());
  if (entry->state != DownloadTracker::State::Finished) return;
  const auto it = replacing_.find(entry->ref.toStdString());
  if (it == replacing_.end()) return;
  // Copied: Confirm runs an event loop in which `entry` and the map can change.
  const Build old = it->second;
  replacing_.erase(it);
  if (notify::Confirm(window_, "Update finished",
                      QString("Games that used %1 now use %2. Remove %1?")
                          .arg(QString::fromStdString(old.label), downloads_->NameFor(*entry)),
                      "Remove", /*destructive=*/true)) {
    api::DeleteRunnerAsync(this, old.kind, old.name, [this](RunnerRemoveResult result) {
      if (!result.ok)
        notify::FailedRequest(window_, "Could not remove the old build.", result.error);
    });
  }
}

}  // namespace mira_gui
