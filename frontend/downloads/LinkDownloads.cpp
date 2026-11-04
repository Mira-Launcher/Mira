#include "LinkDownloads.h"

#include <QApplication>
#include <algorithm>
#include <json.hpp>

#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/api/Downloads.h"

namespace mira_gui {

LinkDownloads::LinkDownloads(QObject* parent) : QObject(parent) {
  connect(
      EventHub::Instance(), &EventHub::Received, this,
      [this](const std::string& type, const std::string& data, bool live) {
        // Replayed history is older than what Load reads.
        if (!live || (type != "download.changed" && type != "download.removed")) return;
        const auto payload = nlohmann::json::parse(data, nullptr, false);
        if (!payload.is_object()) return;
        if (type == "download.changed") return Store(api::ParseDownload(payload));
        const std::string id = payload.value("id", std::string());
        if (std::erase_if(downloads_, [&id](const DownloadInfo& d) { return d.id == id; }) > 0) {
          emit Removed(QString::fromStdString(id));
          emit NoticeChanged();
        }
      });
}

void LinkDownloads::Load() {
  api::GetDownloadsAsync(this, [this](DownloadsResult result) {
    if (!result.ok) return;
    std::vector<std::string> gone;
    for (const DownloadInfo& old : downloads_) {
      if (std::ranges::none_of(result.downloads,
                               [&old](const DownloadInfo& d) { return d.id == old.id; }))
        gone.push_back(old.id);
    }
    downloads_ = std::move(result.downloads);
    for (const std::string& id : gone) emit Removed(QString::fromStdString(id));
    emit Changed(QString());
    emit NoticeChanged();
  });
}

void LinkDownloads::Store(DownloadInfo info) {
  const QString id = QString::fromStdString(info.id);
  const auto found = std::ranges::find(downloads_, info.id, &DownloadInfo::id);
  const bool new_state = found == downloads_.end() || found->state != info.state;
  if (new_state) seen_failures_.erase(info.id);
  if (found == downloads_.end()) {
    downloads_.push_back(std::move(info));
  } else {
    *found = std::move(info);
  }
  emit Changed(id);
  if (new_state) emit NoticeChanged();
}

const DownloadInfo* LinkDownloads::Find(const std::string& id) const {
  const auto found = std::ranges::find(downloads_, id, &DownloadInfo::id);
  return found == downloads_.end() ? nullptr : &*found;
}

std::vector<DownloadInfo> LinkDownloads::WaitingForPassword() const {
  std::vector<DownloadInfo> waiting;
  for (const DownloadInfo& download : downloads_) {
    if (download.state == "password" && !skipped_.contains(download.id))
      waiting.push_back(download);
  }
  return waiting;
}

int LinkDownloads::UnseenFailures() const {
  return static_cast<int>(std::ranges::count_if(downloads_, [this](const DownloadInfo& download) {
    return download.state == "failed" && !seen_failures_.contains(download.id);
  }));
}

void LinkDownloads::SeenFailures() {
  for (const DownloadInfo& download : downloads_) {
    if (download.state == "failed") seen_failures_.insert(download.id);
  }
  emit NoticeChanged();
}

void LinkDownloads::ClearFinished() {
  for (const DownloadInfo& download : downloads_) {
    if (download.state == "done") Remove(download.id);
  }
  SeenFailures();
}

void LinkDownloads::Skip(const std::string& id) {
  skipped_.insert(id);
  emit NoticeChanged();
}

void LinkDownloads::Unskip(const std::string& id) {
  skipped_.erase(id);
  emit NoticeChanged();
}

void LinkDownloads::Failed(const QString& what, const ApiError& error) {
  notify::FailedRequest(QApplication::activeWindow(), what, error);
}

void LinkDownloads::Act(const std::string& id, const std::string& action) {
  api::DownloadActionAsync(this, id, action, [this, action](StoreActionResult result) {
    if (!result.ok)
      Failed(QString("Could not %1 the download.").arg(QString::fromStdString(action)),
             result.error);
  });
}

void LinkDownloads::Remove(const std::string& id) {
  api::RemoveDownloadAsync(this, id, [this](StoreActionResult result) {
    if (!result.ok) Failed("Could not remove the download.", result.error);
  });
}

void LinkDownloads::SubmitPassword(const std::string& id, const std::string& password) {
  api::SubmitDownloadPasswordAsync(this, id, password, [this](StoreActionResult result) {
    if (!result.ok) Failed("Could not try that password.", result.error);
  });
}

}  // namespace mira_gui
