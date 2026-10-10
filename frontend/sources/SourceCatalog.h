#pragma once

#include <QString>
#include <QWidget>

#include <map>
#include <vector>

#include "Sources.h"

class QLabel;
class QPushButton;

namespace mira_gui {

class SourceTile;

// The sources not set up yet, as tiles grouped by kind, with a chip per
// group. Each tile says what setting it up takes and has the one button
// that starts it.
class SourceCatalog : public QWidget {
  Q_OBJECT

public:
  explicit SourceCatalog(QWidget* parent = nullptr);

  // The sources to offer, in AllSources' order. Rebuilds the tiles.
  void SetSources(const std::vector<SourceInfo>& sources);
  // Shows only the tiles whose name, kind or description contains `text`.
  void SetFilter(const QString& text);

signals:
  void SetUpRequested(QString id);

private:
  struct Group {
    QString name;
    QPushButton* chip = nullptr;
    QLabel* heading = nullptr;
    QWidget* grid = nullptr;
  };

  void ApplyFilter();

  std::vector<Group> groups_;
  QPushButton* all_ = nullptr;
  QLabel* empty_ = nullptr;
  std::map<QString, SourceTile*> tiles_;   // by source id
  std::map<QString, int> group_of_;        // by source id
  std::map<QString, QString> haystack_;    // by source id: name, kind and blurb, lowercased
  QString filter_;
};

}  // namespace mira_gui
