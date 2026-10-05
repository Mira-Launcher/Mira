#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QMenu;
class QPushButton;
class QToolButton;

namespace mira_gui {

// A page's tabs, each with a count, then the page's own actions at the end.
// At most one tab is current; none is when the page shows something no tab
// names (a filter picked from elsewhere).
//
// When the row gets narrow, the search box gives way first (320 px down to
// 160, then a search button), then the last tabs move into a More menu. The
// current tab always stays in the row, and nothing is cut.
class TabRow : public QWidget {
  Q_OBJECT

public:
  // The height of a search box or the filter pill beside the tabs.
  static constexpr int kControlHeight = 30;

  explicit TabRow(QWidget* parent = nullptr);

  void AddTab(const QString& key, const QString& label);
  void SetCount(const QString& key, int count);
  // Draws the count in the warning color, for a tab that wants a look.
  void SetAlert(const QString& key, bool alert);
  void SetCurrent(const QString& key);
  QString Current() const;
  void SetTabsVisible(bool visible);
  // Appends `widget` after the tabs, right-aligned.
  void SetTrailing(QWidget* widget);
  // The page's search box, last in the row and the first to shrink.
  void SetSearch(QLineEdit* search);
  // Shows the search box even when the row only has room for its button.
  void OpenSearch();

signals:
  void CurrentChanged(QString key);

protected:
  bool event(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  struct Tab {
    QPushButton* button = nullptr;
    QString label;
    int count = -1;
    bool alert = false;
  };
  void Relabel(Tab& tab);
  void Select(const QString& key);
  // Sizes the search box and decides which tabs fit, for the current width.
  void Fit();

  QHBoxLayout* tabs_ = nullptr;
  QHBoxLayout* layout_ = nullptr;
  QHash<QString, Tab> by_key_;
  QStringList order_;  // tab keys, left to right
  QString current_;
  bool tabs_visible_ = true;
  QList<QWidget*> trailing_;
  QPushButton* more_ = nullptr;
  QMenu* more_menu_ = nullptr;
  QLineEdit* search_ = nullptr;
  QToolButton* search_button_ = nullptr;
  bool search_open_ = false;  // opened from its button while the row is narrow
  bool fitting_ = false;
};

// "● " in `color`, for a status line.
QString StatusDot(const QColor& color);

}  // namespace mira_gui
