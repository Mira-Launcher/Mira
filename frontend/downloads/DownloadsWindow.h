#pragma once

#include <QString>
#include <QWidget>
#include <map>
#include <set>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

class DownloadTracker;
class LinkDownloads;
class ProgressRail;
struct DownloadInfo;

// The full view of downloads, opened from Activity: speed, limit, the queue with each game's files,
// and what store installs are doing.
class DownloadsWindow : public QWidget {
  Q_OBJECT

 public:
  DownloadsWindow(LinkDownloads* downloads, DownloadTracker* tracker, QWidget* parent);
  void Open();

 protected:
  void changeEvent(QEvent* event) override;

 private:
  // What a row's progress can update without rebuilding, which would take a typed password with it.
  struct Parts {
    QLabel* detail = nullptr;
    ProgressRail* rail = nullptr;
    std::vector<QLabel*> files;
  };

  void Refresh();
  void Rebuild();
  void UpdateRows();
  void UpdateHeader();
  std::string Shape() const;
  void AddSection(const QString& heading);
  QWidget* BuildRow(const DownloadInfo& download);
  QWidget* BuildStoreRow(const QString& key);
  void LoadSettings();
  void EditPasswords();
  void Save(const char* key, int value);
  void PaintChrome();

  LinkDownloads* downloads_;
  DownloadTracker* tracker_;
  QVBoxLayout* rows_ = nullptr;
  QLabel* speed_ = nullptr;
  QPushButton* pause_all_ = nullptr;
  QSpinBox* limit_ = nullptr;
  QSpinBox* parallel_ = nullptr;
  QToolButton* minimize_ = nullptr;
  QToolButton* maximize_ = nullptr;
  QToolButton* close_ = nullptr;
  std::set<std::string> expanded_;
  std::map<std::string, QString> typed_;  // passwords being typed, across rebuilds
  std::map<std::string, Parts> parts_;
  std::string shape_;  // the rows on screen
};

}  // namespace mira_gui
