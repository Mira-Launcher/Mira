#include "AppearancePreviews.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <array>
#include <utility>

#include "../library/ArtworkStore.h"
#include "../library/GameTileDelegate.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

// A sidebar strip and a row of covers, in `colors`.
void PaintMiniWindow(QPainter* painter, const QRectF& rect, const theme::Tokens& colors,
                     const std::vector<GameSummary>& games, ArtworkStore* artwork, qreal dpr) {
  painter->setPen(QPen(colors.border, 1));
  painter->setBrush(colors.window);
  painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

  const QRectF sidebar(rect.left() + 1, rect.top() + 1, 32, rect.height() - 2);
  painter->setPen(Qt::NoPen);
  painter->setBrush(colors.surface);
  painter->drawRect(sidebar);
  painter->setPen(QPen(colors.border, 1));
  painter->drawLine(sidebar.topRight(), sidebar.bottomRight());
  painter->setPen(Qt::NoPen);
  for (int i = 0; i < 4; ++i) {
    painter->setBrush(i == 0 ? colors.accent : colors.border);
    painter->drawRoundedRect(QRectF(sidebar.left() + 5, sidebar.top() + 7 + i * 9, sidebar.width() - 10, 5), 2, 2);
  }

  const QRectF grid = rect.adjusted(sidebar.width() + 9, 8, -8, -8);
  constexpr int kCovers = 3;
  const qreal gap = 5;
  const qreal width = (grid.width() - gap * (kCovers - 1)) / kCovers;
  const QSize cover(qRound(width), qRound(width * 1.5));
  const std::array<QColor, 4> samples = theme::SampleArt(colors);
  for (int i = 0; i < kCovers; ++i) {
    const QRectF at(grid.left() + i * (width + gap), grid.top(), width, width * 1.5);
    if (games.empty()) {
      painter->setBrush(samples[i]);
      painter->drawRoundedRect(at, 3, 3);
    } else if (i < static_cast<int>(games.size())) {
      painter->drawPixmap(at.toRect(), artwork->Cover(games[i], cover, dpr));
    }
  }
}

}  // namespace

// --- ThemeChoice ----------------------------------------------------------

ThemeChoice::ThemeChoice(const QString& theme, const QString& label, std::vector<GameSummary> games,
                         ArtworkStore* artwork, QWidget* parent)
    : QAbstractButton(parent), theme_(theme), label_(label), games_(std::move(games)), artwork_(artwork) {
  setCheckable(true);
  setAttribute(Qt::WA_Hover);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(label);
  setFixedSize(176, 132);
  connect(artwork_, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
  LoadTokens();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    LoadTokens();
    update();
  });
}

void ThemeChoice::LoadTokens() {
  tokens_ = theme::Peek(theme_ == "auto" ? QString("mira-dark") : theme_);
  if (theme_ == "auto") light_ = theme::Peek("mira-light");
}

void ThemeChoice::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
  painter.setPen(QPen(isChecked() ? tokens.accent : tokens.border, isChecked() ? 2 : 1));
  painter.setBrush(isChecked() || underMouse() ? tokens.surface_alt : tokens.surface);
  painter.drawRoundedRect(frame, tokens.radius_control + 2, tokens.radius_control + 2);
  if (hasFocus()) {
    painter.setPen(QPen(tokens.accent, 1, Qt::DotLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame.adjusted(3, 3, -3, -3), tokens.radius_control, tokens.radius_control);
  }

  const QRectF preview(12, 12, width() - 24, 86);
  const qreal dpr = devicePixelRatioF();
  if (theme_ == "auto") {
    // Dark above the diagonal, light below: following the desktop means either.
    for (const bool light : {false, true}) {
      painter.save();
      QPainterPath half;
      if (light) {
        half.addPolygon(QPolygonF({preview.topRight(), preview.bottomRight(), preview.bottomLeft()}));
      } else {
        half.addPolygon(QPolygonF({preview.topLeft(), preview.topRight(), preview.bottomLeft()}));
      }
      half.closeSubpath();
      painter.setClipPath(half);
      PaintMiniWindow(&painter, preview, light ? light_ : tokens_, games_, artwork_, dpr);
      painter.restore();
    }
  } else {
    PaintMiniWindow(&painter, preview, tokens_, games_, artwork_, dpr);
  }

  QFont font = this->font();
  font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  painter.setPen(tokens.text);
  painter.drawText(QRect(12, 106, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   painter.fontMetrics().elidedText(label_, Qt::ElideRight, width() - 24));
}

// --- TilePreview ----------------------------------------------------------

namespace {
constexpr QSize kPreviewTile(104, 156);
}

TilePreview::TilePreview(std::vector<GameSummary> games, ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent), model_(new QStandardItemModel(this)), delegate_(new GameTileDelegate(this, kPreviewTile, artwork)) {
  using Role = GameTileDelegate::Role;
  // No games yet: two sample tiles, the second from a store so its source shows.
  samples_ = games.empty();
  if (samples_) {
    for (const auto& [name, source] :
         {std::pair{"Sample game", "local"}, std::pair{"Another game", "steam"}}) {
      GameSummary game;
      game.name = name;
      game.source = source;
      games.push_back(game);
    }
  }
  // The second playing, so the status line has a tile to show on.
  for (size_t i = 0; i < games.size() && i < 2; ++i) {
    auto* item = new QStandardItem();
    item->setData(QString::fromStdString(games[i].id), Role::IdRole);
    item->setData(QString::fromStdString(games[i].name), Role::NameRole);
    item->setData(QString("ready"), Role::StatusRole);
    item->setData(i == 1, Role::RunningRole);
    item->setData(QString::fromStdString(games[i].source), Role::SourceRole);
    model_->appendRow(item);
  }
  setFixedSize(sizeHint());
  connect(artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
}

void TilePreview::SetShown(bool status, bool source) {
  delegate_->SetShowStatus(status);
  delegate_->SetShowSource(source);
  update();
}

QSize TilePreview::sizeHint() const { return {kPreviewTile.width() * 2, kPreviewTile.height()}; }

void TilePreview::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const std::array<QColor, 4> colors = theme::SampleArt(theme::Current());
  for (int row = 0; row < model_->rowCount(); ++row) {
    if (samples_) {
      // A ready cover, so the delegate asks the artwork store for nothing.
      // Colored here rather than once, to follow a theme change.
      QPixmap cover(kPreviewTile);
      cover.fill(colors[row]);
      const QSignalBlocker block(model_);
      model_->setData(model_->index(row, 0), cover, Qt::DecorationRole);
    }
    QStyleOptionViewItem option;
    option.initFrom(this);
    option.state &= ~(QStyle::State_MouseOver | QStyle::State_HasFocus);
    option.rect = QRect(QPoint(row * kPreviewTile.width(), 0), kPreviewTile);
    option.font = font();
    delegate_->paint(&painter, option, model_->index(row, 0));
  }
}

// --- LayoutPreview --------------------------------------------------------

LayoutPreview::LayoutPreview(std::vector<GameSummary> games, ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent), games_(std::move(games)), artwork_(artwork) {
  setMinimumHeight(128);
  connect(artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
}

void LayoutPreview::SetShape(const Shape& shape) {
  shape_ = shape;
  update();
}

QSize LayoutPreview::sizeHint() const { return {520, 128}; }

void LayoutPreview::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(QPen(tokens.border, 1));
  painter.setBrush(tokens.window);
  painter.drawRoundedRect(panel, shape_.panel_radius, shape_.panel_radius);

  // The Play button, in the control rounding.
  const QFontMetrics metrics(font());
  const QRectF play(width() - 16 - (metrics.horizontalAdvance("Play") + 42), height() / 2.0 - 15,
                    metrics.horizontalAdvance("Play") + 42, 30);
  painter.setPen(Qt::NoPen);
  painter.setBrush(tokens.accent);
  painter.drawRoundedRect(play, shape_.control_radius, shape_.control_radius);
  icons::For(icons::Glyph::Play, tokens.on_accent).paint(&painter, QRect(play.left() + 10, play.center().y() - 7, 14, 14));
  painter.setPen(tokens.on_accent);
  painter.drawText(play.adjusted(28, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, "Play");

  // Covers inside the padding, with the gap between them, clipped to the panel.
  const int pad = std::max(shape_.grid_padding, 0);
  const qreal cover_height = height() - 2.0 * pad - 2;
  const qreal cover_width = cover_height * 2 / 3;
  if (cover_height <= 8) return;
  QPainterPath inside;
  inside.addRoundedRect(panel, shape_.panel_radius, shape_.panel_radius);
  painter.setClipPath(inside);
  const qreal dpr = devicePixelRatioF();
  const std::array<QColor, 4> samples = theme::SampleArt(tokens);
  const size_t count = games_.empty() ? 8 : games_.size();  // samples fill the row
  qreal x = 1 + pad;
  for (size_t i = 0; i < count; ++i) {
    if (x + cover_width > play.left() - 12) break;
    const QRectF at(x, 1 + pad, cover_width, cover_height);
    QPainterPath shape;
    shape.addRoundedRect(at, shape_.cover_radius, shape_.cover_radius);
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);
    if (games_.empty()) {
      painter.fillRect(at, samples[i % samples.size()]);
    } else {
      painter.drawPixmap(at.toRect(), artwork_->Cover(games_[i], at.size().toSize(), dpr));
    }
    painter.restore();
    x += cover_width + shape_.tile_gap;
  }
}

}  // namespace mira_gui
