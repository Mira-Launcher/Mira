#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

#include "../theme/Icons.h"

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVariantAnimation;
class QVBoxLayout;

namespace mira_gui {

class SettingsCard;
class SettingsPage;

// Row indices grouped by category, in first-seen order. Never sorted: the
// schema's order is the display order.
struct CategoryRows {
  QString name;
  std::vector<size_t> rows;
};
std::vector<CategoryRows> GroupByCategory(const std::vector<std::string>& categories);

// Where a settings category sits in the nav: its group heading and icon. An
// unknown category (one mirad added later) lands under Games with a generic icon.
QString CategoryNavGroup(const QString& category);
icons::Glyph CategoryGlyph(const QString& category);

// Left-nav-plus-search chrome shared by the Settings screen and a game's own
// settings: grouped category rows and search on the left, every category's
// page in one scroll on the right. The nav jumps to a page and follows the
// scroll; the current page's title sticks to the top. Knows nothing about config schemas:
// each consumer's own gate (the overridable filter) composes with the live
// search via SetRowGateVisible instead of both fighting over a row's visibility.
class SettingsNavWidget : public QWidget {
  Q_OBJECT

public:
  explicit SettingsNavWidget(QWidget* parent = nullptr);

  // A page with its nav row, under `nav_group`'s heading (none when empty).
  SettingsPage* AddCategory(const QString& title, icons::Glyph glyph, const QString& nav_group = {});

  // Call once per row, after it is placed in a card on one of these pages, so
  // the search box can find it. Every query word must appear, in any order
  // (ui/SettingsSearch).
  void RegisterRow(QWidget* row_widget, const QString& searchable_text);

  // A second visibility gate under the search filter, e.g. the overridable
  // flag in a game's settings. Defaults to true.
  void SetRowGateVisible(QWidget* row_widget, bool visible);

  void SetNavWidth(int width);

  // Clears the search, unfolds row_widget's card and scrolls it into view.
  // Leaves focus to the caller.
  void RevealRow(QWidget* row_widget);
  // Scrolls `page` to the top and highlights it in the nav.
  void RevealPage(SettingsPage* page);

  // Above the search box, e.g. the screen's back button and title.
  void SetHeaderWidget(QWidget* widget);

  // The page area, for a change bar to float over.
  QWidget* ContentArea() const;

  // Extra space under the last page while a change bar floats over it.
  void SetBottomRoom(int height);

  // Splits every page's cards into columns again, e.g. once loaded values
  // have given them their real heights. Folding never does this by itself.
  void RearrangePages();

  // Back to the first page's top, at once.
  void ScrollToTop();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Category {
    QPushButton* button = nullptr;
    QLabel* count = nullptr;          // matches while searching, inside the button
    QLabel* group_heading = nullptr;  // the nav heading this category sits under, if any
    SettingsPage* page = nullptr;
    icons::Glyph glyph = icons::Glyph::Dot;
    int total_rows = 0;
  };

  struct RowEntry {
    QWidget* row_widget = nullptr;
    SettingsCard* card = nullptr;
    QString search_text;  // settings_search::Normalize'd
    bool gate_visible = true;
    int category_index = -1;
  };

  void ApplyFilter();
  // Scrolls to a page's title.
  void Select(int index);
  void RefreshIcons();
  // The sticky title, the nav's current row and the room under the last page,
  // after a scroll or a change in the pages' size.
  void SyncToScroll();
  int TitleTop(const Category& category) const;
  int CurrentIndex() const;

  QLineEdit* search_ = nullptr;
  QWidget* nav_ = nullptr;
  QVBoxLayout* nav_layout_ = nullptr;
  QButtonGroup* buttons_ = nullptr;
  QScrollArea* scroll_ = nullptr;            // every page, one after another
  QWidget* canvas_ = nullptr;
  QVBoxLayout* sections_ = nullptr;
  QLabel* sticky_ = nullptr;                 // the current page's title, over the scroll's top
  QVariantAnimation* jump_ = nullptr;
  int jumping_to_ = -1;                      // the nav row a jump is heading for
  int bottom_room_ = 0;                      // for a change bar
  QStackedWidget* content_stack_ = nullptr;  // scroll_ vs. empty_state_
  QLabel* empty_state_ = nullptr;            // shown when a query matches nothing at all
  QWidget* left_ = nullptr;
  QVBoxLayout* left_layout_ = nullptr;  // header widget (if any), search box, nav
  QWidget* header_widget_ = nullptr;
  QLabel* current_heading_ = nullptr;   // the group heading new categories go under
  QString current_group_;

  std::vector<Category> categories_;
  std::vector<RowEntry> rows_;
  QHash<QWidget*, int> row_index_by_widget_;  // row_widget -> index into rows_
  QHash<SettingsCard*, bool> folded_before_search_;
};

}  // namespace mira_gui
