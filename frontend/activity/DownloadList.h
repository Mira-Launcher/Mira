#pragma once

#include <QString>
#include <QWidget>
#include <functional>
#include <map>

#include "DownloadTracker.h"

class QBoxLayout;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;

// DownloadTracker's work as rows, newest first: a cover, the name, what it's doing, a progress rail
// while it runs, and its buttons. The Activity popover shows all of it compactly; the Downloads
// page shows one section per list, with what links need besides (a password box, cancel, move
// ahead).
class DownloadList : public QWidget {
  Q_OBJECT

 public:
  enum class Style { Compact, Full };
  DownloadList(DownloadTracker* tracker, ArtworkStore* artwork, Style style,
               QWidget* parent = nullptr);

  // Which entries this list shows; all of them when unset.
  void SetFilter(std::function<bool(const DownloadTracker::Entry&)> filter);
  // Rows by this rank, lowest first; unset keeps the tracker's order (newest first).
  void SetOrder(std::function<int(const DownloadTracker::Entry&)> rank);
  void Rebuild();
  int Count() const { return count_; }
  // The rows' height at `width`, up to `rows` of them.
  int HeightFor(int width, int rows) const;

 signals:
  void ShowGameRequested(const QString& id);
  void LogRequested(const QString& channel, const QString& title);
  // An archive waiting to be extracted.
  void ArchiveSetupRequested(const QString& path);
  // A link download: pause, resume, retry or front.
  void LinkAction(const QString& id, const QString& action);
  void LinkPasswordRequested(const QString& id);
  void LinkPasswordEntered(const QString& id, const QString& password);
  void LinkRemoveRequested(const QString& id);
  // A button that leaves the list for somewhere else (a game, a fix).
  void Leaving();
  // Rows were added or removed.
  void Rebuilt();

 private:
  QWidget* BuildRow(const DownloadTracker::Entry& entry, bool first);
  void AddLinkActions(const DownloadTracker::Entry& entry, QWidget* row, QBoxLayout* actions,
                      QVBoxLayout* text);
  void UpdateCover(const QString& id);
  // Refreshes a still-running row in place; false when it needs a Rebuild.
  bool UpdateRow(const QString& key);

  DownloadTracker* tracker_;
  ArtworkStore* artwork_;
  Style style_;
  std::function<bool(const DownloadTracker::Entry&)> filter_;
  std::function<int(const DownloadTracker::Entry&)> rank_;
  QVBoxLayout* rows_ = nullptr;
  int count_ = 0;
  std::map<QString, QString> typed_;  // a password half typed, by link id, across rebuilds
};

}  // namespace mira_gui
