#include "TitleFilterBar.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>

#include <algorithm>
#include <array>

#include "../sources/Sources.h"
#include "../widgets/TabRow.h"

namespace mira_gui {
namespace {

// Picking several values shouldn't close the dropdown after each one.
class StayOpenMenu : public QMenu {
public:
  using QMenu::QMenu;

protected:
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (QAction* action = actionAt(event->position().toPoint()); action != nullptr && action->isCheckable()) {
      action->trigger();
      return;
    }
    QMenu::mouseReleaseEvent(event);
  }
};

DropdownButton* Dropdown(QWidget* parent, QMenu* menu) {
  auto* button = new DropdownButton(menu, parent);
  button->setObjectName("chip");
  return button;
}

QString StoreName(const QString& id) {
  const SourceInfo* info = FindSourceInfo(id);
  return info != nullptr ? info->name : id;
}

// "Any", the one value, or how many.
template <typename Set, typename Name>
QString Picked(const Set& picked, const QString& several, Name name) {
  if (picked.empty()) return "Any";
  if (picked.size() == 1) return name(*picked.begin());
  return QString("%1 %2").arg(picked.size()).arg(several);
}

// Indexed by TitleFilterBar::Sort: the menu's label and the pref's key.
constexpr std::array<std::pair<const char*, const char*>, 4> kSorts = {
    {{"Store order", "store"}, {"Name", "name"}, {"Best reviewed", "reviews"}, {"Best on ProtonDB", "protondb"}}};

int TierRank(const std::string& tier) {
  const std::string filter_tier = ProtonDbFilterTier(tier);
  const auto* found = std::ranges::find_if(kProtonDbFilterTiers, [&](const char* t) { return filter_tier == t; });
  return static_cast<int>(found - kProtonDbFilterTiers.begin());
}

}  // namespace

TitleFilterBar::TitleFilterBar(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  store_menu_ = new StayOpenMenu(this);
  store_ = Dropdown(this, store_menu_);
  layout->addWidget(store_);

  auto* tier_menu = new StayOpenMenu(this);
  for (const char* tier : kProtonDbFilterTiers) {
    QAction* action = tier_menu->addAction(ProtonDbTierName(tier));
    action->setCheckable(true);
    connect(action, &QAction::toggled, this, [this, value = std::string(tier)](bool on) {
      if (on) filter_.tiers.insert(value); else filter_.tiers.erase(value);
      Update();
    });
  }
  tier_ = Dropdown(this, tier_menu);
  tier_->setToolTip("How well each game runs through Proton, from ProtonDB's reports.");
  layout->addWidget(tier_);

  auto* review_menu = new StayOpenMenu(this);
  for (const ReviewBucket bucket : kReviewBuckets) {
    QAction* action = review_menu->addAction(ReviewBucketName(bucket));
    action->setCheckable(true);
    connect(action, &QAction::toggled, this, [this, bucket](bool on) {
      if (on) filter_.reviews.insert(bucket); else filter_.reviews.erase(bucket);
      Update();
    });
  }
  review_ = Dropdown(this, review_menu);
  review_->setToolTip("Steam's user reviews. Games from other stores use the Steam game of the same name while "
                      "matching by name is on.");
  layout->addWidget(review_);

  auto* sort_menu = new QMenu(this);
  auto* sorts = new QActionGroup(sort_menu);
  for (size_t i = 0; i < kSorts.size(); ++i) {
    QAction* action = sort_menu->addAction(kSorts[i].first);
    action->setCheckable(true);
    action->setChecked(static_cast<Sort>(i) == sort_);
    sorts->addAction(action);
    sort_actions_.push_back(action);
    connect(action, &QAction::triggered, this, [this, sort = static_cast<Sort>(i)] {
      sort_ = sort;
      Update();
      emit SortChanged();
    });
  }
  sort_button_ = Dropdown(this, sort_menu);
  layout->addWidget(sort_button_);

  clear_ = new QPushButton("Clear filters", this);
  clear_->setObjectName("text_button");
  QSizePolicy keep = clear_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  clear_->setSizePolicy(keep);
  connect(clear_, &QPushButton::clicked, this, &TitleFilterBar::Clear);
  layout->addWidget(clear_);
  layout->addStretch(1);
  Update();
}

void TitleFilterBar::SetStores(const std::vector<QString>& stores) {
  if (stores == stores_) return;
  stores_ = stores;
  store_menu_->clear();
  for (const QString& store : stores_) {
    QAction* action = store_menu_->addAction(StoreName(store));
    action->setCheckable(true);
    action->setChecked(filter_.stores.contains(store.toStdString()));
    connect(action, &QAction::toggled, this, [this, value = store.toStdString()](bool on) {
      if (on) filter_.stores.insert(value); else filter_.stores.erase(value);
      Update();
    });
  }
  Update();
}

bool TitleFilterBar::Matches(const OwnedMatch& match) const {
  return std::ranges::any_of(match.copies, [&](const auto& copy) {
    return filter_.Matches(copy.first.toStdString(), match.protondb_tier, match.review_summary);
  });
}

void TitleFilterBar::SortMatches(std::vector<OwnedMatch>& matches) const {
  switch (sort_) {
    case Sort::StoreOrder:
      return;
    case Sort::Name:
      std::ranges::stable_sort(matches, [](const OwnedMatch& a, const OwnedMatch& b) {
        return QString::localeAwareCompare(a.title, b.title) < 0;
      });
      return;
    case Sort::BestReviewed:
      // Unreviewed last.
      std::ranges::stable_sort(matches, std::greater<>(), &OwnedMatch::review_percent);
      return;
    case Sort::ProtonDb:
      std::ranges::stable_sort(matches, {}, [](const OwnedMatch& match) { return TierRank(match.protondb_tier); });
      return;
  }
}

std::string TitleFilterBar::SortKey() const {
  return kSorts[static_cast<size_t>(sort_)].second;
}

void TitleFilterBar::SetSortKey(const std::string& key) {
  const auto* found = std::ranges::find(kSorts, key, [](const auto& sort) { return std::string(sort.second); });
  sort_ = found == kSorts.end() ? Sort::StoreOrder : static_cast<Sort>(found - kSorts.begin());
  sort_actions_[static_cast<size_t>(sort_)]->setChecked(true);
  Update();
}

void TitleFilterBar::Clear() {
  for (DropdownButton* button : {store_, tier_, review_}) {
    for (QAction* action : button->Menu()->actions()) {
      const QSignalBlocker block(action);
      action->setChecked(false);
    }
  }
  filter_ = {};
  Update();
}

void TitleFilterBar::Update() {
  store_->setText("Store: " + Picked(filter_.stores, "stores", [](const std::string& id) {
    return StoreName(QString::fromStdString(id));
  }));
  tier_->setText("ProtonDB: " + Picked(filter_.tiers, "tiers", ProtonDbTierName));
  review_->setText("Steam reviews: " + Picked(filter_.reviews, "labels", ReviewBucketName));
  sort_button_->setText(QString("Sort: ") + kSorts[static_cast<size_t>(sort_)].first);
  clear_->setVisible(!filter_.Empty());
  emit Changed();
}

}  // namespace mira_gui
