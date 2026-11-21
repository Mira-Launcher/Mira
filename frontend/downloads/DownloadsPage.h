#pragma once

#include <QWidget>
#include <vector>

class QLabel;
class QPushButton;

namespace mira_gui {

class ArtworkStore;
class DownloadList;
class DownloadTracker;
class LinkDownloads;
class PageHeader;
class SettingsCard;

// Every download and install, as a page of the window like Runners: what needs the user, what's
// running, what waits, and what finished, each in a card of its own. The speed limit and the rest
// live in Settings › Downloads, one click away.
class DownloadsPage : public QWidget {
  Q_OBJECT

 public:
  DownloadsPage(LinkDownloads* links, DownloadTracker* tracker, ArtworkStore* artwork,
                QWidget* parent = nullptr);

 signals:
  void CloseRequested();  // a row opened something else, which the page makes way for
  void SettingsRequested();
  void ShowGameRequested(const QString& id);
  void LogRequested(const QString& channel, const QString& title);

 private:
  struct Section {
    SettingsCard* card = nullptr;
    DownloadList* list = nullptr;
  };

  void Refresh();
  void UpdateHeader();
  void EditPasswords();

  LinkDownloads* links_;
  DownloadTracker* tracker_;
  std::vector<Section> sections_;
  PageHeader* header_ = nullptr;  // its subtitle sums up what runs
  QLabel* empty_ = nullptr;
  QPushButton* pause_all_ = nullptr;
};

}  // namespace mira_gui
