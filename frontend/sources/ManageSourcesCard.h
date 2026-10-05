#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <string>
#include <vector>

#include "SettingsCard.h"
#include "Sources.h"

class QLabel;
class QPushButton;
class QToolButton;

namespace mira_gui {

// Every source in one list, in sidebar order: drag a row (or Alt+Up/Down) to
// reorder, the switch turns a source on or off, and ⋯ holds open, import,
// sidebar visibility and remove. Each change applies at once, so there is no
// Save; the window stores each one as it's made.
class ManageSourcesCard : public SettingsCard {
  Q_OBJECT

public:
  struct Entry {
    SourceInfo source;
    bool ready = false;
    bool enabled = true;
    int games = 0;
    bool in_sidebar = true;
    std::string account;           // signed-in account, if the source says
    std::int64_t imported_at = 0;  // unix seconds; 0 if never
  };

  explicit ManageSourcesCard(QWidget* parent = nullptr);

  // Entries in sidebar order. The first call builds the rows; later ones
  // update and reorder them in place, keeping any import in progress.
  void SetEntries(const std::vector<Entry>& entries);

signals:
  void OpenRequested(QString id);
  void SidebarToggled(QString id, bool shown);
  void EnabledToggled(QString id, bool enabled);
  void OrderChanged(QStringList ids);
  void Imported(QString id);  // an import finished and changed something
  void Removed(QString id);
  void CloseRequested();

private:
  struct Row {
    Entry entry;
    SettingRow* row = nullptr;
    QLabel* badge = nullptr;
    QLabel* status = nullptr;
    Switch* enabled = nullptr;
    QPushButton* set_up = nullptr;
    QToolButton* more = nullptr;
    bool importing = false;
    QString note;  // an import's result, shown instead of the status from then on
  };

  void BuildRow(const Entry& entry);
  void Update(Row& row);
  void ShowMenu(const QString& id);
  void Import(const QString& id);
  Row* Find(const QString& id);

  std::vector<Row> rows_;
};

}  // namespace mira_gui
