#pragma once

#include <QString>
#include <QStringList>
#include <QFrame>

#include <vector>

#include "AddedSources.h"

class QButtonGroup;
class QLabel;
class QLineEdit;
class QResizeEvent;
class QScrollArea;
class QStackedWidget;

namespace mira_gui {

class ModalOverlay;
class SourceCatalog;
class SourceSetupCard;

// Sources, as a card over the library: "Added" shows the sources that are
// set up, "Add source" the rest, and one search box filters either. A source
// moves from one to the other as it's set up or removed.
class SourcesPage : public QFrame {
  Q_OBJECT

public:
  explicit SourcesPage(QWidget* parent = nullptr);

  // Every source, in sidebar order, as Sidebar::SourceEntries gives them.
  void SetEntries(const std::vector<SourceEntry>& entries);
  // Shows the catalog, the search focused.
  void ShowCatalog();
  void ShowAdded();
  // Whether the set-up dialog is up, and closing it (Esc).
  bool SetupOpen() const;
  void CloseSetup();

signals:
  void OpenRequested(QString id);
  void SettingsRequested(QString id);
  void SidebarToggled(QString id, bool shown);
  void EnabledToggled(QString id, bool enabled);
  void Imported(QString id);
  void Removed(QString id);
  void CloseRequested();

private:
  QButtonGroup* views_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QLabel* hint_ = nullptr;
  QLineEdit* search_ = nullptr;
  AddedSources* added_ = nullptr;
  SourceCatalog* catalog_ = nullptr;
  std::vector<SourceEntry> entries_;
  ModalOverlay* setup_overlay_ = nullptr;
  QScrollArea* setup_scroll_ = nullptr;
  SourceSetupCard* setup_card_ = nullptr;

  // Lutris imports at once; other sources get a "Set up" dialog over the page.
  void OpenSetup(const QString& id);
  void FitSetup();
  void ShowView(int view);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
};

}  // namespace mira_gui
