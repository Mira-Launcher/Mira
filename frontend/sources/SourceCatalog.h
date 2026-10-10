#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <vector>

#include "Sources.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace mira_gui {

class SettingRow;
class SettingsCard;

// The sources not set up yet, as rows lined up like the Added list's: a
// search box, filter chips, then one card for game sources and one for apps.
class SourceCatalog : public QWidget {
  Q_OBJECT

public:
  explicit SourceCatalog(QWidget* parent = nullptr);

  // The sources to offer, in AllSources' order. Rebuilds the rows.
  void SetSources(const std::vector<SourceInfo>& sources);
  void FocusSearch();

signals:
  void SetUpRequested(QString id);

private:
  void ApplyFilter();

  QLineEdit* search_ = nullptr;
  QPushButton* all_ = nullptr;    // the chips, one checked at a time
  QPushButton* games_ = nullptr;
  QPushButton* apps_ = nullptr;
  SettingsCard* games_card_ = nullptr;
  SettingsCard* apps_card_ = nullptr;
  QLabel* empty_ = nullptr;
  QHash<QString, SettingRow*> rows_;  // by source id
  QHash<QString, QString> haystack_;  // by source id: name, kind, blurb and tool, lowercased
  std::vector<SourceInfo> sources_;
};

}  // namespace mira_gui
