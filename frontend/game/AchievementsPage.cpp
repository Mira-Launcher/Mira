#include "AchievementsPage.h"

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "../client/api/Artwork.h"
#include "../components/ActivationKey.h"
#include "../components/FlowLayout.h"
#include "../components/IconButton.h"
#include "../components/ProgressRail.h"
#include "../components/Scrolling.h"
#include "../components/Text.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "HeroBackdrop.h"

namespace mira_gui {

namespace {
constexpr int kIconSize = 56;
constexpr int kTileWidth = 240;  // the narrowest a tile gets before a column goes
constexpr double kRare = 10.0;   // percent of players, under which rarity is marked

std::string IconKey(const Achievement& item) {
  return item.key + (item.unlocked ? "\nunlocked\n" : "\nlocked\n") + item.icon;
}
}  // namespace

// The icon in a rounded square: a blank square until it loads, dimmed while locked, a lock while
// it's a hidden one.
class AchievementIcon : public QWidget {
 public:
  explicit AchievementIcon(QWidget* parent) : QWidget(parent) {
    setFixedSize(kIconSize, kIconSize);
    connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
            qOverload<>(&QWidget::update));
  }
  void Set(const QPixmap& icon, bool locked, bool masked) {
    icon_ = icon;
    locked_ = locked;
    masked_ = masked;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()), tokens.radius_control, tokens.radius_control);
    painter.setClipPath(clip);
    if (masked_ || icon_.isNull()) {
      painter.fillRect(rect(), tokens.surface_alt);
      if (masked_) {
        const int glyph = kIconSize * 2 / 5;
        icons::For(icons::Glyph::Lock, tokens.text_muted)
            .paint(&painter, QRect((width() - glyph) / 2, (height() - glyph) / 2, glyph, glyph));
      }
      return;
    }
    if (locked_) painter.setOpacity(0.55);
    painter.drawPixmap(rect(), icon_);
  }

 private:
  QPixmap icon_;
  bool locked_ = false;
  bool masked_ = false;
};

AchievementTile::AchievementTile(const Achievement& item, QWidget* parent)
    : QFrame(parent), item_(item) {
  // Its column's width, whatever its text would like: the grid sets the columns.
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(10, 10, 10, 10);
  row->setSpacing(10);
  icon_ = new AchievementIcon(this);
  row->addWidget(icon_, 0, Qt::AlignTop);
  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  name_ = new QLabel(this);
  SetTextStyle(name_, text::kSection);
  name_->setWordWrap(true);
  description_ = new QLabel(this);
  SetTextStyle(description_, text::kDetail);
  description_->setWordWrap(true);
  // Wraps rather than widen the tile past its column.
  auto* facts_box = new QWidget(this);
  auto* facts = new FlowLayout(facts_box, 10);
  when_ = new QLabel(facts_box);
  SetTextStyle(when_, text::kDetail);
  rarity_ = new QLabel(facts_box);
  SetTextStyle(rarity_, text::kDetail);
  facts->addWidget(when_);
  facts->addWidget(rarity_);
  text->addWidget(name_);
  text->addWidget(description_);
  text->addSpacing(2);
  text->addWidget(facts_box);
  text->addStretch(1);
  row->addLayout(text, 1);
  Fill();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &AchievementTile::Fill);
}

bool AchievementTile::Masked() const {
  return item_.hidden && !item_.unlocked && !revealed_;
}

void AchievementTile::SetIcon(const QPixmap& icon) {
  pixmap_ = icon;
  icon_->Set(pixmap_, !item_.unlocked, Masked());
}

void AchievementTile::mouseReleaseEvent(QMouseEvent* event) {
  QFrame::mouseReleaseEvent(event);
  if (event->button() == Qt::LeftButton) Reveal();
}

void AchievementTile::keyPressEvent(QKeyEvent* event) {
  const int key = event->key();
  if (Masked() && IsActivationKey(key)) return Reveal();
  QFrame::keyPressEvent(event);
}

void AchievementTile::Reveal() {
  if (!Masked()) return;
  revealed_ = true;
  Fill();
  emit Revealed();
}

void AchievementTile::Fill() {
  const bool masked = Masked();
  setCursor(masked ? Qt::PointingHandCursor : Qt::ArrowCursor);
  setFocusPolicy(masked ? Qt::TabFocus : Qt::NoFocus);
  name_->setText(masked ? "Hidden achievement" : QString::fromStdString(item_.name));
  description_->setText(masked ? "Click to show it" : QString::fromStdString(item_.description));
  description_->setVisible(!description_->text().isEmpty());
  when_->setText(item_.unlocked && item_.unlocked_at > 0
                     ? QLocale().toString(QDateTime::fromSecsSinceEpoch(item_.unlocked_at).date(),
                                          "d MMM yyyy")
                 : item_.unlocked ? "Unlocked"
                                  : "Locked");
  rarity_->setVisible(item_.rarity >= 0);
  rarity_->setText(QString("%1% of players").arg(QLocale().toString(item_.rarity, 'f', 1)));
  const bool rare = item_.rarity >= 0 && item_.rarity < kRare;
  SetTextStyle(rarity_, {TextRole::Body, rare ? Tone::Warning : Tone::Muted, TextSize::Small});
  icon_->Set(pixmap_, !item_.unlocked, masked);
}

AchievementsPage::AchievementsPage(std::string id, QWidget* parent)
    : QWidget(parent), id_(std::move(id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  controls_ = new QWidget(this);
  auto* controls = new QVBoxLayout(controls_);
  controls->setContentsMargins(0, 0, 0, 0);
  controls->setSpacing(8);
  count_ = new QLabel(controls_);
  rail_ = new ProgressRail(controls_);
  controls->addWidget(count_);
  controls->addWidget(rail_);
  controls_->hide();  // until the host places it

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SeeThrough(scroll_);
  SetUpScrolling(scroll_, this);
  auto* content = new QWidget();
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(kPageContentMargins);
  grid_ = new QGridLayout();
  grid_->setSpacing(10);
  content_layout->addLayout(grid_);
  content_layout->addStretch(1);
  scroll_->setWidget(content);
  layout->addWidget(scroll_, 1);
}

void AchievementsPage::Show(const AchievementsResult& set) {
  count_->setText(QString("%1 of %2 unlocked").arg(set.unlocked).arg(set.total));
  rail_->SetProgress(set.total > 0 ? static_cast<double>(set.unlocked) / set.total : 0.0);

  // Icons whose item went or changed its pictures are dropped.
  std::map<std::string, QPixmap> kept;
  for (const Achievement& item : set.items) {
    if (auto found = icons_.find(IconKey(item)); found != icons_.end()) kept.insert(*found);
  }
  icons_ = std::move(kept);

  for (AchievementTile* tile : tiles_) tile->deleteLater();
  tiles_.clear();
  columns_ = 0;
  for (const Achievement& item : set.items) {
    auto* tile = new AchievementTile(item, scroll_->widget());
    tiles_.push_back(tile);
    connect(tile, &AchievementTile::Revealed, this, [this, tile] { LoadIcon(tile); });
    LoadIcon(tile);
  }
  Arrange();
}

void AchievementsPage::LoadIcon(AchievementTile* tile) {
  const Achievement& item = tile->item();
  // A hidden one's picture can give it away, so it waits for the click.
  if (item.icon.empty() || tile->Masked()) return;
  if (auto found = icons_.find(IconKey(item)); found != icons_.end())
    return tile->SetIcon(found->second);
  icon_queue_.emplace_back(tile);
  NextIcons();
}

void AchievementsPage::NextIcons() {
  while (icon_requests_ < kIconRequests && !icon_queue_.empty()) {
    const QPointer<AchievementTile> tile = icon_queue_.front();
    icon_queue_.pop_front();
    if (tile.isNull()) continue;  // gone with a refresh
    const Achievement& item = tile->item();
    ++icon_requests_;
    // The page, not the tile, so a tile gone meanwhile still frees its turn.
    api::GetAchievementIconAsync(
        this, id_, item.key, item.unlocked, [this, tile, key = IconKey(item)](QImage image) {
          --icon_requests_;
          NextIcons();
          if (image.isNull()) return;
          const QPixmap icon = QPixmap::fromImage(
              image.scaled(kIconSize * devicePixelRatio(), kIconSize * devicePixelRatio(),
                           Qt::KeepAspectRatio, Qt::SmoothTransformation));
          icons_[key] = icon;
          if (!tile.isNull()) tile->SetIcon(icon);
        });
  }
}

void AchievementsPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  Arrange();
}

void AchievementsPage::Arrange() {
  // From the page's own width: the viewport's lags behind until the page has been shown.
  const int room = width() - 36;
  const int columns = std::max(1, (room + grid_->spacing()) / (kTileWidth + grid_->spacing()));
  if (columns == columns_ && grid_->count() == static_cast<int>(tiles_.size())) return;
  columns_ = columns;
  while (grid_->count() > 0) delete grid_->takeAt(0);
  for (int column = 0; column < grid_->columnCount(); ++column) grid_->setColumnStretch(column, 0);
  for (std::size_t index = 0; index < tiles_.size(); ++index) {
    grid_->addWidget(tiles_[index], static_cast<int>(index) / columns,
                     static_cast<int>(index) % columns);
  }
  for (int column = 0; column < columns; ++column) grid_->setColumnStretch(column, 1);
}

}  // namespace mira_gui
