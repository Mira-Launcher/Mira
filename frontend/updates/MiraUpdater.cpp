#include "MiraUpdater.h"

#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <json.hpp>
#include <vector>

#include "../activity/DownloadTracker.h"
#include "../app/Display.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/Jobs.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../library/GameLibraryModel.h"
#include "../system/PackageInstall.h"
#include "../widgets/Labels.h"

namespace mira_gui {
namespace {

constexpr int kPollMs = 500;
constexpr int kGameAttempts = 60;    // a game's own shutdown and its saves' upload
constexpr int kDaemonAttempts = 20;  // mirad finishes what it can, then stops

std::vector<const GameSummary*> RunningGames(const GameLibraryModel& library) {
  std::vector<const GameSummary*> running;
  for (const GameSummary& game : library.Games()) {
    if (game.running) running.push_back(&game);
  }
  return running;
}

bool SavesSyncing(const DownloadTracker& downloads) {
  return std::ranges::any_of(downloads.Entries(), [](const DownloadTracker::Entry& entry) {
    return entry.kind == DownloadTracker::Kind::Job && entry.source == "saves" &&
           entry.state == DownloadTracker::State::Running;
  });
}

}  // namespace

MiraUpdater::MiraUpdater(GameLibraryModel* library, DownloadTracker* downloads, QWidget* window)
    : QObject(window), library_(library), downloads_(downloads), window_(window) {
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (!live || state_ != State::Downloading || !type.starts_with("job.")) return;
            const auto payload = nlohmann::json::parse(data, nullptr, false);
            if (!payload.is_object()) return;
            const std::string id = payload.value("id", std::string());
            if (type == "job.started" && payload.value("kind", std::string()) == "mira_update") {
              job_ = id;
            } else if (type == "job.progress" && id == job_ && !job_.empty()) {
              percent_ = payload.value("done", 0);
              message_ = QString::fromStdString(payload.value("message", std::string()));
              emit Progress();
            }
          });
}

void MiraUpdater::SetState(State state, const QString& error) {
  state_ = state;
  error_ = error;
  if (state != State::Downloading) job_.clear();
  emit Changed();
}

void MiraUpdater::Start(const QString& latest) {
  if (Busy()) return;
  QStringList closing;
  for (const GameSummary* game : RunningGames(*library_))
    closing << QString::fromStdString(game->name);
  const int installs = downloads_->RunningCount();
  if (!closing.isEmpty() || installs > 0) {
    QString question;
    if (!closing.isEmpty())
      question +=
          QString("Updating closes %1, and anything unsaved is lost. ").arg(JoinAnd(closing));
    if (installs > 0)
      question +=
          installs == 1
              ? QString("An install or download still running is cancelled. ")
              : QString("%1 installs or downloads still running are cancelled. ").arg(installs);
    question += "Mira restarts when it's done.";
    const QString accept = closing.isEmpty()     ? QString("Update")
                           : closing.size() == 1 ? QString("Close game and update")
                                                 : QString("Close games and update");
    if (!notify::Confirm(window_, "Update Mira to " + latest + "?", question, accept,
                         /*destructive=*/true))
      return;
  }
  percent_ = 0;
  message_.clear();
  SetState(State::Downloading);
  api::UpdateMiraAsync(this, [this](MiraUpdateResult result) {
    if (!result.ok) {
      // Cancelling is no failure: the row offers Update again.
      if (result.error.code == "cancelled") return SetState(State::Idle);
      return SetState(State::Failed, error_help::Describe(result.error));
    }
    if (result.install.empty()) return CloseGames();  // an AppImage, already swapped
    Install(result.install);
  });
}

void MiraUpdater::Cancel() {
  if (state_ != State::Downloading || job_.empty()) return;
  jobs::Cancel(this, job_, [this](ApiError error) {
    if (!error.message.empty()) notify::FailedRequest(window_, "Could not cancel it.", error);
  });
}

void MiraUpdater::Install(const std::vector<std::string>& command) {
  SetState(State::Installing);
  system::InstallPackages(window_, command, "the Mira update", /*restart=*/false,
                          [this](bool installed) {
                            if (installed) CloseGames();
                            // Dismissed, or told how to run it by hand: nothing else changed.
                            else
                              SetState(State::Idle);
                          });
}

void MiraUpdater::CloseGames() {
  SetState(State::Closing);
  for (const GameSummary* game : RunningGames(*library_)) {
    api::StopGameAsync(this, game->id, [](StopResult) {});
  }
  WaitForGames(kGameAttempts);
}

void MiraUpdater::WaitForGames(int attempts_left) {
  if (attempts_left <= 0 || (RunningGames(*library_).empty() && !SavesSyncing(*downloads_)))
    return Restart();
  QTimer::singleShot(kPollMs, this, [this, attempts_left] { WaitForGames(attempts_left - 1); });
}

void MiraUpdater::Restart() {
  SetState(State::Restarting);
  api::ShutdownDaemonAsync(this, [this] { WaitForDaemon(kDaemonAttempts); });
}

void MiraUpdater::WaitForDaemon(int attempts_left) {
  api::CheckHealthAsync(this, [this, attempts_left](HealthStatus status) {
    if (status.reachable && attempts_left > 0) {
      QTimer::singleShot(kPollMs, this,
                         [this, attempts_left] { WaitForDaemon(attempts_left - 1); });
      return;
    }
    // Nothing to hand over: the new build starts its own mirad.
    display::SetDaemon({}, {});
    if (!display::Restart({}))
      SetState(State::Failed, "Mira is updated, but couldn't restart. Close it and open it again.");
  });
}

}  // namespace mira_gui
