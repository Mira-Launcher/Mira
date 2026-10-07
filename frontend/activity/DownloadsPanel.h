#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class DownloadTracker;

// The top bar's downloads popover: what DownloadTracker knows, newest
// first. A Qt::Popup, so it closes itself on an outside click or Escape.
class DownloadsPanel : public QWidget {
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

private:
  void Rebuild();
  QWidget* BuildRow(int index);
  // Redraws the covers showing `id`.
  void UpdateCover(const QString& id);
  // Refreshes a still-running row in place; false when it needs a Rebuild.
  bool UpdateRow(const QString& key);

  DownloadTracker* tracker_;
  ArtworkStore* artwork_;
  QVBoxLayout* rows_ = nullptr;
  QLabel* empty_ = nullptr;
  QPushButton* clear_ = nullptr;
};

}  // namespace mira_gui
