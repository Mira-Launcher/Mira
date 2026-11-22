#include "RatingChips.h"

#include <QGridLayout>

#include "../components/Chip.h"
#include "../components/FlowLayout.h"
#include "../components/Labels.h"
#include "GamePresentation.h"

namespace mira_gui {
namespace {}  // namespace

RatingChips::RatingChips(QWidget* parent) : QWidget(parent) {
  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(6);
  grid->setColumnStretch(1, 1);

  const auto row = [&](int at, const QString& label, const QString& help) {
    auto* name = MakeLabel(this, label, text::kDetail, /*wrap=*/false);
    name->setFixedWidth(104);
    name->setToolTip(help);
    grid->addWidget(name, at, 0, Qt::AlignTop);
    auto* chips = new QWidget(this);
    (new FlowLayout(chips, 6))->SetPreferOneLine(true);
    grid->addWidget(chips, at, 1);
    return chips;
  };
  const auto chip = [this](QWidget* row_widget) {
    auto* button = new Chip(QString(), row_widget);
    button->setCheckable(true);
    row_widget->layout()->addWidget(button);
    connect(button, &Chip::toggled, this, &RatingChips::Toggle);
    return button;
  };

  QWidget* tiers =
      row(0, "ProtonDB", "How well each game runs through Proton, from ProtonDB's reports.");
  for (const char* tier : kProtonDbFilterTiers) {
    Chip* button = chip(tiers);
    if (*tier != '\0') button->SetTint(ProtonDbTierColor(tier));
    tier_chips_[tier] = button;
  }
  QWidget* reviews =
      row(1, "Steam reviews",
          "Steam's user reviews. Games from other stores use the Steam game of the same name while "
          "matching by name is on.");
  for (const ReviewBucket bucket : kReviewBuckets) {
    review_chips_[bucket] = chip(reviews);
    review_chips_[bucket]->SetTint(ReviewBucketColor(bucket));
  }
  SetTitles({});
}

void RatingChips::SetTitles(const std::vector<Title>& titles) {
  std::map<std::string, int> tiers;
  std::map<ReviewBucket, int> reviews;
  for (const Title& title : titles) {
    ++tiers[ProtonDbFilterTier(title.protondb_tier)];
    ++reviews[ReviewBucketOf(title.review_summary)];
  }
  for (const auto& [tier, button] : tier_chips_) {
    button->setText(ProtonDbTierName(tier));
    button->SetCount(tiers[tier]);
  }
  for (const auto& [bucket, button] : review_chips_) {
    button->setText(ReviewBucketName(bucket));
    button->SetCount(reviews[bucket]);
  }
}

void RatingChips::Clear() {
  for (const auto& [tier, button] : tier_chips_) {
    const QSignalBlocker block(button);
    button->setChecked(false);
  }
  for (const auto& [bucket, button] : review_chips_) {
    const QSignalBlocker block(button);
    button->setChecked(false);
  }
  Toggle();
}

void RatingChips::Toggle() {
  filter_.tiers.clear();
  filter_.reviews.clear();
  for (const auto& [tier, button] : tier_chips_) {
    if (button->isChecked()) filter_.tiers.insert(tier);
  }
  for (const auto& [bucket, button] : review_chips_) {
    if (button->isChecked()) filter_.reviews.insert(bucket);
  }
  emit Changed();
}

}  // namespace mira_gui
