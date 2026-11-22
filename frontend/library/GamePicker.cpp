#include "GamePicker.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

#include "../components/Button.h"
#include "../components/FlowLayout.h"
#include "../components/IconButton.h"
#include "../components/Labels.h"
#include "../components/Scrolling.h"
#include "../components/SearchField.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "ArtworkStore.h"
#include "ZoomSettle.h"

namespace mira_gui {

// One game's cover, ticked or not; a click toggles it.
class PickTile : public QAbstractButton {
 public:
  static constexpr int kName = 20;

  PickTile(const GameSummary& game, ArtworkStore* artwork, int width, QWidget* parent)
      : QAbstractButton(parent), game_(game), artwork_(artwork), width_(width) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QString::fromStdString(game.name));
    setAccessibleName(QString::fromStdString(game.name));
  }

  const GameSummary& Game() const { return game_; }
  void SetWidth(int width) {
    if (width == width_) return;
    width_ = width;
    updateGeometry();
    update();
  }
  // The library grid's tile shape, so the covers it already scaled are reused.
  QSize CoverSize() const { return {width_, width_ * 3 / 2}; }
  QSize sizeHint() const override { return {width_, CoverSize().height() + 4 + kName}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF cover(QPointF(0, 0), CoverSize());
    const qreal radius = tokens.radius_tile;
    painter.save();
    if (!isChecked()) painter.setOpacity(0.55);
    QPainterPath clip;
    clip.addRoundedRect(cover, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(cover.toRect(), artwork_->Cover(game_, CoverSize(), devicePixelRatioF()));
    painter.restore();

    const QRectF box(width_ - 26, 6, 20, 20);
    if (isChecked()) {
      painter.setPen(QPen(tokens.accent, 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(cover.adjusted(1, 1, -1, -1), radius, radius);
      painter.setPen(Qt::NoPen);
      painter.setBrush(tokens.accent);
      painter.drawRoundedRect(box, 5, 5);
      painter.setPen(QPen(tokens.on_accent, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPolyline(
          QPolygonF({box.topLeft() + QPointF(5, 10), box.topLeft() + QPointF(8.5, 13.5),
                     box.topLeft() + QPointF(15, 6.5)}));
    } else {
      QColor shade = tokens.window;
      shade.setAlphaF(0.6);
      painter.setBrush(shade);
      painter.setPen(QPen(tokens.text, 1));
      painter.drawRoundedRect(box.adjusted(1, 1, -1, -1), 5, 5);
    }

    painter.setPen(isChecked() ? tokens.text : tokens.text_muted);
    const QRect name(0, CoverSize().height() + 4, width_, kName);
    painter.drawText(
        name, Qt::AlignLeft | Qt::AlignVCenter,
        fontMetrics().elidedText(QString::fromStdString(game_.name), Qt::ElideRight, width_));
  }

 private:
  GameSummary game_;
  ArtworkStore* artwork_;
  int width_;
};

GamePicker::GamePicker(ArtworkStore* artwork, const QString& back_label, QWidget* parent)
    : QWidget(parent), artwork_(artwork) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  header_box_ = new QWidget(this);
  header_box_->setObjectName("settings_canvas");
  header_ = new QHBoxLayout(header_box_);
  header_->setContentsMargins(24, 20, 32, 10);
  header_->setSpacing(8);
  // As Settings' own back arrow.
  auto* back = new IconButton(icons::Glyph::ArrowLeft, back_label, header_box_);
  connect(back, &QToolButton::clicked, this, &GamePicker::Closed);
  header_->addWidget(back);
  title_ = new QLabel(header_box_);
  title_->setObjectName("page_title");
  header_->addWidget(title_);
  header_->addSpacing(4);
  header_->addStretch(1);
  search_ = new SearchField("Search games", header_box_);
  search_->setObjectName("library_search");
  search_->setFixedWidth(240);
  connect(search_, &QLineEdit::textChanged, this, &GamePicker::Filter);
  header_->addWidget(search_);
  outer->addWidget(header_box_);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("settings_page");
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll_, this);
  scroll_->viewport()->installEventFilter(this);
  content_ = new QWidget();
  content_->setObjectName("settings_canvas");
  sections_ = new QVBoxLayout(content_);
  // Room at the bottom so the floating bar never covers the last covers.
  sections_->setContentsMargins(32, 6, 32, 90);
  sections_->setSpacing(10);
  scroll_->setWidget(content_);
  outer->addWidget(scroll_, /*stretch=*/1);

  // Floats over the bottom, like the change bar it looks like.
  bar_ = new QFrame(this);
  bar_->setObjectName("change_bar");
  bar_layout_ = new QHBoxLayout(bar_);
  bar_layout_->setContentsMargins(16, 8, 8, 8);
  bar_layout_->setSpacing(10);
  auto* cancel = new QPushButton("Cancel", bar_);
  connect(cancel, &QPushButton::clicked, this, &GamePicker::Closed);
  bar_layout_->addWidget(cancel);
  apply_ = new Button(QString(), ButtonKind::Primary, bar_);
  apply_->setDefault(true);
  connect(apply_, &QPushButton::clicked, this, [this] { Apply(); });
  bar_layout_->addWidget(apply_);

  zoom_settle_ = new ZoomSettle(this, artwork_, tile_width_, [this] {
    for (PickTile* tile : tiles_) tile->update();
  });
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    for (PickTile* tile : tiles_) {
      if (QString::fromStdString(tile->Game().id) == id) tile->update();
    }
  });
}

bool GamePicker::eventFilter(QObject* watched, QEvent* event) {
  if (watched == scroll_->viewport() && event->type() == QEvent::Wheel) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (wheel->modifiers() & Qt::ControlModifier) {
      if (const int steps = wheel->angleDelta().y() / 120; steps != 0) emit ZoomRequested(steps);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

TileRow GamePicker::Row() const {
  for (const QWidget* grid : content_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
    if (grid->isVisible() && dynamic_cast<const FlowLayout*>(grid->layout()) != nullptr) {
      return {grid->width(), kTileGap};
    }
  }
  return {};
}

void GamePicker::SetTileWidth(int width) {
  tile_width_ = width;
  zoom_settle_->Step(width);
  for (PickTile* tile : tiles_) tile->SetWidth(width);
  // So each heading moves with the covers above it in the same frame.
  SettleLayout(content_);
}

void GamePicker::SetShowingHidden(bool showing) {
  show_hidden_ = showing;
  Filter();
}

void GamePicker::SetTitle(const QString& title) {
  title_->setText(title);
}

void GamePicker::AddHeaderWidget(QWidget* widget) {
  widget->setParent(header_box_);
  header_->insertWidget(header_->count() - 2, widget);  // before the stretch and the search
}

void GamePicker::AddBarWidget(QWidget* widget) {
  widget->setParent(bar_);
  bar_layout_->insertWidget(bar_layout_->count() - 2, widget);  // before Cancel and apply
}

void GamePicker::Clear() {
  tiles_.clear();
  hidden_section_ = nullptr;
  while (QLayoutItem* item = sections_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
}

void GamePicker::AddSection(const QString& heading, const std::vector<const GameSummary*>& games,
                            const std::vector<std::string>& ticked, bool hidden) {
  auto* label = MakeGroupHeading(content_, heading.toUpper());
  sections_->addWidget(label);
  auto* grid = new QWidget(content_);
  auto* flow = new FlowLayout(grid, kTileGap);
  for (const GameSummary* game : games) {
    auto* tile = new PickTile(*game, artwork_, tile_width_, grid);
    tile->setChecked(std::ranges::contains(ticked, game->id));
    connect(tile, &PickTile::toggled, this, &GamePicker::UpdateBar);
    flow->addWidget(tile);
    tiles_.append(tile);
  }
  sections_->addWidget(grid);
  if (hidden) hidden_section_ = grid;
}

void GamePicker::FinishSections() {
  sections_->addStretch(1);
  search_->clear();
  Filter();
  UpdateBar();
}

void GamePicker::Filter() {
  const QString text = search_->text().trimmed();
  for (PickTile* tile : tiles_) {
    tile->setVisible(text.isEmpty() ||
                     QString::fromStdString(tile->Game().name).contains(text, Qt::CaseInsensitive));
  }
  // A section whose every cover is filtered out hides with its heading. Hidden mode shows only the
  // hidden games' section, and only it hides it.
  for (int i = 0; i + 1 < sections_->count(); i += 2) {
    QWidget* heading = sections_->itemAt(i)->widget();
    QWidget* grid = sections_->itemAt(i + 1)->widget();
    if (heading == nullptr || grid == nullptr) continue;
    const bool any = std::ranges::any_of(
        tiles_, [grid](const PickTile* t) { return t->parentWidget() == grid && !t->isHidden(); });
    const bool shown = any && (grid == hidden_section_) == show_hidden_;
    heading->setVisible(shown);
    grid->setVisible(shown);
  }
}

std::vector<std::string> GamePicker::TickedIds() const {
  std::vector<std::string> ids;
  for (const PickTile* tile : tiles_) {
    if (tile->isChecked()) ids.push_back(tile->Game().id);
  }
  return ids;
}

int GamePicker::TickedCount() const {
  return static_cast<int>(std::ranges::count_if(tiles_, &PickTile::isChecked));
}

void GamePicker::UpdateBar() {
  UpdateApply();
  bar_->adjustSize();
  bar_->move((width() - bar_->width()) / 2, height() - bar_->height() - 18);
  bar_->raise();
}

void GamePicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  UpdateBar();
}

void GamePicker::SortAndSplit(std::vector<const GameSummary*>& games,
                              std::vector<const GameSummary*>& hidden) {
  std::ranges::sort(games, [](const GameSummary* a, const GameSummary* b) {
    return QString::fromStdString(a->name).compare(QString::fromStdString(b->name),
                                                   Qt::CaseInsensitive) < 0;
  });
  std::erase_if(games, [&](const GameSummary* game) {
    if (!IsHidden(*game)) return false;
    hidden.push_back(game);
    return true;
  });
}

}  // namespace mira_gui
