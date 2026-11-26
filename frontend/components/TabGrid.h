#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QGridLayout;
class QPushButton;

namespace mira_gui {

// Tabs, each with a count, in rows of `columns` that share the width, for a narrow place where a
// TabRow would fold some into More. Styled like a TabRow's tabs by its object name.
class TabGrid : public QWidget {
  Q_OBJECT

 public:
  explicit TabGrid(int columns, QWidget* parent = nullptr);

  void AddTab(const QString& key, const QString& label);
  void SetCount(const QString& key, int count);
  void SetCurrent(const QString& key);
  QString Current() const { return current_; }

 signals:
  void CurrentChanged(QString key);

 private:
  struct Tab {
    QPushButton* button = nullptr;
    QString label;
  };

  int columns_;
  QGridLayout* grid_ = nullptr;
  QHash<QString, Tab> by_key_;
  QString current_;
};

}  // namespace mira_gui
