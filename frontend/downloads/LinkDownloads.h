#pragma once

#include <QObject>
#include <set>
#include <string>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// The link download queue as mirad reports it, for Activity, the top bar and the Downloads window.
class LinkDownloads : public QObject {
  Q_OBJECT

 public:
  explicit LinkDownloads(QObject* parent = nullptr);

  // Reads the whole list, at startup and after a lost connection.
  void Load();
  const std::vector<DownloadInfo>& Downloads() const { return downloads_; }
  const DownloadInfo* Find(const std::string& id) const;

  // Those the top bar asks about: waiting for a password, and not skipped.
  std::vector<DownloadInfo> WaitingForPassword() const;
  // Those that failed since the Downloads window was last open or Activity last cleared.
  int UnseenFailures() const;
  void SeenFailures();
  // Activity's Clear finished: done ones leave the queue, failed ones stop being news.
  void ClearFinished();
  // Stops asking; the password can still be entered from the Downloads window.
  void Skip(const std::string& id);
  void Unskip(const std::string& id);

  void Act(const std::string& id, const std::string& action);
  void Remove(const std::string& id);
  void SubmitPassword(const std::string& id, const std::string& password);

 signals:
  void Changed(const QString& id);  // empty when the whole list was read
  void Removed(const QString& id);
  // What the top bar's notice depends on may have changed. Not on every tick of progress.
  void NoticeChanged();

 private:
  void Store(DownloadInfo info);
  void Failed(const QString& what, const ApiError& error);

  std::vector<DownloadInfo> downloads_;
  std::set<std::string> skipped_;
  std::set<std::string> seen_failures_;
};

}  // namespace mira_gui
