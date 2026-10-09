#include "RatingChips.h"

#include <QGridLayout>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>

#include "../widgets/FlowLayout.h"
#include "../widgets/Labels.h"
#include "GamePresentation.h"

namespace mira_gui {
namespace {

QIcon Swatch(const QColor& color) {
  QPixmap pixmap(18, 18);
  pixmap.setDevicePixelRatio(2);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawEllipse(QRectF(0, 0, 9, 9));
  return QIcon(pixmap);
}

QString ChipText(const QString& name, int count) { return QString("%1  %2").arg(name).arg(count); }

}  // namespace

RatingChips::RatingChips(QWidget* parent) : QWidget(parent) {
  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(6);
  grid->setColumnStretch(1, 1);

  const auto row = [&](int at, const QString& label, const QString& help) {
    auto* name = MakeLabel(this, label, "muted", /*wrap=*/false);
    name->setFixedWidth(104);
    name->setToolTip(help);
    grid->addWidget(name, at, 0, Qt::AlignTop);
    auto* chips = new QWidget(this);
    new FlowLayout(chips, 6);
    grid->addWidget(chips, at, 1);
    return chips;
  };
  const auto chip = [this](QWidget* row_widget) {
    auto* button = new QPushButton(row_widget);
    button->setObjectName("chip");
    button->setCheckable(true);
    button->setIconSize(QSize(9, 9));
    row_widget->layout()->addWidget(button);
    connect(button, &QPushButton::toggled, this, &RatingChips::Toggle);
    return button;
  };

  QWidget* tiers = row(0, "ProtonDB", "How well each game runs through Proton, from ProtonDB's reports.");
  for (const char* tier : kProtonDbFilterTiers) {
    QPushButton* button = chip(tiers);
    button->setIcon(Swatch(*tier == '\0' ? QColor("#4a4f5a") : ProtonDbTierColor(tier)));
    tier_chips_[tier] = button;
  }
  QWidget* reviews = row(1, "Steam reviews",
                         "Steam's user reviews. Games from other stores use the Steam game of the same name while "
                         "matching by name is on.");
  for (const ReviewBucket bucket : kReviewBuckets) review_chips_[bucket] = chip(reviews);
  SetTitles({});
}

void RatingChips::SetTitles(const std::vector<Title>& titles) {
  std::map<std::string, int> tiers;
  std::map<ReviewBucket, int> reviews;
  for (const Title& title : titles) {
    ++tiers[ProtonDbFilterTier(title.protondb_tier)];
    ++reviews[ReviewBucketOf(title.review_summary)];
  }
  for (const auto& [tier, button] : tier_chips_) button->setText(ChipText(ProtonDbTierName(tier), tiers[tier]));
  for (const auto& [bucket, button] : review_chips_) button->setText(ChipText(ReviewBucketName(bucket), reviews[bucket]));
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
