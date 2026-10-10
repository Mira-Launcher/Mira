#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

#include "ManageSourcesCard.h"

class QButtonGroup;
class QStackedWidget;

namespace mira_gui {

class SourceCatalog;

// The library window's Sources page: "Added" lists the sources that are set
// up, games and apps apart, in sidebar order; "Add source" is the catalog of
// the rest. A source moves from one to the other as it's set up or removed.
class SourcesPage : public QWidget {
  Q_OBJECT

public:
  explicit SourcesPage(QWidget* parent = nullptr);

  // Every source, in sidebar order, as Sidebar::SourceEntries gives them.
  void SetEntries(const std::vector<ManageSourcesCard::Entry>& entries);
  // Shows the catalog, its search focused.
  void ShowCatalog();
  void ShowAdded();

signals:
  void OpenRequested(QString id);
  void SidebarToggled(QString id, bool shown);
  void EnabledToggled(QString id, bool enabled);
  void OrderChanged(QStringList ids);
  void Imported(QString id);
  void Removed(QString id);

private:
  QButtonGroup* views_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  ManageSourcesCard* games_ = nullptr;
  ManageSourcesCard* apps_ = nullptr;
  SourceCatalog* catalog_ = nullptr;
  std::vector<ManageSourcesCard::Entry> entries_;
};

}  // namespace mira_gui
