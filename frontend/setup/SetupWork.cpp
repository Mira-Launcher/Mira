#include "SetupWork.h"

#include <QWidget>
#include <json.hpp>
#include <memory>
#include <utility>

#include "../activity/DownloadTracker.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Config.h"
#include "../client/api/Library.h"
#include "../client/api/Runners.h"
#include "../client/api/Stores.h"
#include "../sources/Sources.h"
#include "../system/PackageInstall.h"

namespace mira_gui {

SetupWork::SetupWork(DownloadTracker* downloads, QObject* parent)
    : QObject(parent), downloads_(downloads) {
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  connect(EventHub::Instance(), &EventHub::RunnersChanged, this, [this] {
    if (!proton_running_) return;
    proton_running_ = false;
    emit Changed();
  });
  connect(downloads_, &DownloadTracker::Changed, this, [this](const QString&) {
    if (proton_running_) emit Changed();
  });
}

void SetupWork::InstallPackages(QWidget* window, const std::vector<std::string>& install,
                                const std::vector<std::string>& packages, bool restart) {
  if (packages.empty() || packages_running_) return;
  packages_running_ = true;
  emit Changed();
  system::InstallPackages(window, install, system::Names(packages), restart, [this](bool) {
    packages_running_ = false;
    emit Changed();
    if (office_waiting_) StartOffice();
  });
}

void SetupWork::DownloadProton() {
  if (proton_running_) return;
  proton_running_ = true;
  emit Changed();
  api::GetRunnerCatalogAsync(this, "proton", "", [this](RunnerCatalogResult catalog) {
    if (!catalog.ok || catalog.releases.empty()) {
      proton_running_ = false;
      return emit Changed();
    }
    const RunnerRelease& newest = catalog.releases.front();
    api::DownloadRunnerAsync(this, "proton", newest.tag, newest.source,
                             [this](RunnerDownloadResult result) {
                               if (result.ok) return;  // RunnersChanged ends it
                               proton_running_ = false;
                               emit Changed();
                             });
  });
}

void SetupWork::SetUpTools(const QStringList& stores) {
  for (const QString& store : stores) {
    const SourceInfo* source = FindSourceInfo(store);
    if (source == nullptr || source->kind != SourceInfo::Kind::Store || store == "steam" ||
        tools_running_.contains(store))
      continue;
    tools_running_.insert(store);
    api::GetStoreStatusAsync(this, store.toStdString(), [this, store](StoreStatusResult status) {
      const auto done = [this, store](StoreActionResult) {
        tools_running_.remove(store);
        emit Changed();
      };
      if (status.ok && status.tool_installed) return done({});
      emit Changed();
      api::SetupStoreToolAsync(this, store.toStdString(), done);
    });
  }
}

void SetupWork::PrepareOffice(QWidget* window) {
  if (office_started_) return;
  office_started_ = true;
  office_accepted_ = false;
  office_failed_ = false;
  office_window_ = window;
  if (packages_running_) {
    office_waiting_ = true;
    return emit Changed();
  }
  StartOffice();
}

void SetupWork::StartOffice() {
  office_waiting_ = false;
  // Installed above unless unticked or refused; then this asks once more, for what Office needs.
  system::EnsurePackages(office_window_, "winetricks", "Microsoft 365", [this](bool ready) {
    if (!ready) {
      office_started_ = false;
      office_failed_ = true;
      office_apps_.clear();
      return emit Changed();
    }
    office_running_ = true;
    office_progress_ = -1;
    emit Changed();
    api::InstallLauncherAsync(this, "office", [this](StoreActionResult started) {
      if (started.ok) {
        office_accepted_ = true;
        return AddOfficeApps(std::exchange(office_apps_, {}));
      }
      office_running_ = false;
      office_started_ = false;
      office_failed_ = true;
      office_apps_.clear();
      emit Changed();
    });
  });
}

void SetupWork::AddOfficeApps(const std::vector<std::string>& refs) {
  if (refs.empty()) return;
  // Until mirad has taken the install, it has nothing to queue the apps behind.
  if (office_started_ && !office_accepted_) {
    office_apps_.insert(office_apps_.end(), refs.begin(), refs.end());
    return;
  }
  office_running_ = true;
  emit Changed();
  api::AddOfficeAppsAsync(this, refs, [this](StoreActionResult result) {
    if (result.ok) return;
    office_running_ = false;
    office_failed_ = true;
    emit Changed();
  });
}

void SetupWork::ImportSteam() {
  if (steam_running_) return;
  steam_running_ = true;
  emit Changed();
  api::ScanSteamAsync(this, [this](SteamScanResult) {
    steam_running_ = false;
    emit Changed();
    emit SourcesChanged();
  });
}

void SetupWork::ImportLutris() {
  if (lutris_running_) return;
  lutris_running_ = true;
  emit Changed();
  api::ImportLutrisAsync(this, [this](LutrisImportResult) {
    lutris_running_ = false;
    emit Changed();
    emit SourcesChanged();
  });
}

void SetupWork::SetSourcesOn(const QStringList& on, const QStringList& off) {
  std::vector<std::pair<QString, bool>> edits;
  for (const auto& [ids, value] : {std::pair{&on, true}, std::pair{&off, false}}) {
    for (const QString& id : *ids) {
      if (id != "local") edits.emplace_back(id, value);
    }
  }
  if (edits.empty()) return;
  // One PATCH per source; the sidebar refreshes once the last one lands.
  auto pending = std::make_shared<int>(static_cast<int>(edits.size()));
  for (const auto& [id, enabled] : edits) {
    api::PatchSourceAsync(this, id.toStdString(), enabled, std::nullopt, [this, pending](PatchConfigResult) {
      if (--*pending == 0) emit SourcesChanged();
    });
  }
}

QStringList SetupWork::Running() const {
  QStringList running;
  if (packages_running_) running << "Installing packages";
  if (proton_running_) {
    QString line = "Downloading Proton";
    for (const DownloadTracker::Entry& entry : downloads_->Entries()) {
      if (entry.kind == DownloadTracker::Kind::Runner &&
          entry.state == DownloadTracker::State::Running && entry.progress >= 0)
        line = QString("Proton · %1%").arg(qRound(entry.progress * 100));
    }
    running << line;
  }
  if (steam_running_) running << "Adding Steam games";
  if (lutris_running_) running << "Adding Lutris games";
  if (!tools_running_.isEmpty()) {
    QStringList names;
    for (const QString& store : tools_running_) names << FindSourceInfo(store)->name;
    names.sort();
    running << "Getting " + names.join(", ") + " ready";
  }
  if (office_waiting_) {
    running << "Microsoft 365, after the packages";
  } else if (office_running_) {
    running << (office_progress_ >= 0
                    ? QString("Microsoft 365 · %1%").arg(qRound(office_progress_ * 100))
                    : QString("Microsoft 365"));
  }
  return running;
}

void SetupWork::HandleEvent(const std::string& type, const std::string& data) {
  StoreEvent event;
  if (events::ParseStoreEvent(type, data, &event) && event.kind == "setup" &&
      event.source == "office") {
    if (event.state == "progress") {
      office_progress_ = event.progress;
      emit Changed();
    } else if (event.state == "finished") {
      office_progress_ = -1;
      emit SourcesChanged();
    } else if (event.state == "failed") {
      office_running_ = false;
      office_failed_ = true;
      emit Changed();
    }
    return;
  }
  // The install job ends after the apps picked meanwhile are added, and so does adding more.
  if (type != "job.finished" && type != "job.failed") return;
  const nlohmann::json job = nlohmann::json::parse(data, nullptr, false);
  if (!job.is_object() || job.value("kind", "") != "install" || job.value("target", "") != "office")
    return;
  office_running_ = false;
  if (type == "job.failed") office_failed_ = true;
  emit Changed();
  emit SourcesChanged();
}

}  // namespace mira_gui
