#pragma once

#include <QString>

#include "../components/Popover.h"

class QLabel;
class QPushButton;
class QScrollArea;

namespace mira_gui {

class ArtworkStore;
class DownloadTracker;

// The top bar's downloads popover: what DownloadTracker knows, newest
// first. A Qt::Popup, so it closes itself on an outside click or Escape.
class DownloadsPanel : public Popover {
  Q_OBJECT

 public:
  DownloadsPanel(DownloadTracker* tracker, ArtworkStore* artwork, QWidget* parent);

  // Shows it with its top-right corner under `anchor`'s bottom-right.
  void ShowBelow(QWidget* anchor);

 signals:
  // "Show" on a finished install: the game it became.
  void ShowGameRequested(const QString& id);
  // "Log" on any row: its live log, by channel, with the row's name.
  void LogRequested(const QString& channel, const QString& title);
  // A link download's row: pause, resume or retry.
  void LinkAction(const QString& id, const QString& action);
  void LinkPasswordRequested(const QString& id);
  void ManageRequested();
  void FinishedCleared();

 private:
  void Fit();

  DownloadTracker* tracker_;
  class DownloadList* list_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QLabel* empty_ = nullptr;
  QPushButton* clear_ = nullptr;
};

}  // namespace mira_gui
