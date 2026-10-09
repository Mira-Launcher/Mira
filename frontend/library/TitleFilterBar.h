#pragma once

#include <QString>
#include <QWidget>

#include <vector>

#include "OwnedTitles.h"
#include "TitleRatings.h"

class QAction;
class QMenu;
class QPushButton;

namespace mira_gui {

class DropdownButton;

// The Library's Not installed tab's filters: Store, ProtonDB and Steam reviews as dropdowns that
// each pick any number of values, and the sort.
class TitleFilterBar : public QWidget {
  Q_OBJECT

public:
  enum class Sort { StoreOrder, Name, BestReviewed, ProtonDb };

  explicit TitleFilterBar(QWidget* parent = nullptr);

  // The stores the titles come from, as the Store dropdown offers them.
  void SetStores(const std::vector<QString>& stores);
  const TitleFilter& Filter() const { return filter_; }
  // Whether a title passes the filter; it's from every store it has a copy on.
  bool Matches(const OwnedMatch& match) const;
  // Orders `matches` by the picked sort; the stores' own order is the order given.
  void SortMatches(std::vector<OwnedMatch>& matches) const;
  // The sort as the not_installed_sort pref spells it; an unknown key keeps the stores' order.
  std::string SortKey() const;
  void SetSortKey(const std::string& key);
  void Clear();

signals:
  void Changed();
  void SortChanged();  // picked from the menu

private:
  void Update();

  TitleFilter filter_;
  Sort sort_ = Sort::StoreOrder;
  std::vector<QAction*> sort_actions_;  // in Sort's order
  DropdownButton* store_ = nullptr;
  DropdownButton* tier_ = nullptr;
  DropdownButton* review_ = nullptr;
  DropdownButton* sort_button_ = nullptr;
  QPushButton* clear_ = nullptr;
  QMenu* store_menu_ = nullptr;
  std::vector<QString> stores_;
};

}  // namespace mira_gui
