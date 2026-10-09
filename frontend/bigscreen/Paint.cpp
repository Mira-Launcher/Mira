#include "Paint.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

#include "../library/GamePresentation.h"
#include "../sources/Sources.h"
#include "../theme/Theme.h"

namespace mira_gui::bigscreen {

QFont Font(double unit, double size, int weight) {
  QFont font = QApplication::font();
  font.setPixelSize(std::max(1, qRound(unit * size)));
  font.setWeight(QFont::Weight(weight));
  return font;
}

namespace {
QColor g_accent;
}  // namespace

QColor Accent() { return g_accent.isValid() ? g_accent : theme::Current().accent; }

void SetAccent(const QColor& color) { g_accent = color; }

namespace {
bool g_swap_confirm = false;

// With the face buttons swapped, Select is labelled with the button that now selects.
Nav PhysicalNav(Nav nav, const QString& kind) {
  if (!g_swap_confirm || kind == "keys") return nav;
  return nav == Nav::Accept ? Nav::Back : nav == Nav::Back ? Nav::Accept : nav;
}
}  // namespace

void SetSwapConfirm(bool swap) { g_swap_confirm = swap; }

QString GlyphText(Nav nav, const QString& kind) {
  nav = PhysicalNav(nav, kind);
  if (kind == "keys") {
    switch (nav) {
      case Nav::Accept: return "Enter";
      case Nav::Back: return "Esc";
      case Nav::Action: return "X";
      case Nav::Search: return "Y";
      case Nav::PrevTab: return "Q";
      case Nav::NextTab: return "E";
      case Nav::Sort: return "S";
      case Nav::PrevLetter: return "[";
      case Nav::NextLetter: return "]";
      default: return {};
    }
  }
  const bool ps = kind == "ps", nin = kind == "nin";
  switch (nav) {
    case Nav::Accept: return ps ? "✕" : nin ? "B" : "A";
    case Nav::Back: return ps ? "○" : nin ? "A" : "B";
    case Nav::Action: return ps ? "□" : nin ? "Y" : "X";
    case Nav::Search: return ps ? "△" : nin ? "X" : "Y";
    case Nav::PrevTab: return ps ? "L1" : nin ? "L" : "LB";
    case Nav::NextTab: return ps ? "R1" : nin ? "R" : "RB";
    case Nav::Sort: return ps ? "Create" : nin ? "−" : "View";
    case Nav::PrevLetter: return ps ? "L2" : nin ? "ZL" : "LT";
    case Nav::NextLetter: return ps ? "R2" : nin ? "ZR" : "RT";
    default: return {};
  }
}

namespace {
bool Wide(Nav nav) {
  return nav == Nav::PrevTab || nav == Nav::NextTab || nav == Nav::PrevLetter || nav == Nav::NextLetter;
}

// The small button left of center: Xbox's View (two overlapping squares) or PlayStation's Create
// (a pill with three rays). Nintendo's minus is plain text.
void DrawSortIcon(QPainter& painter, const QRectF& rect, double unit, bool ps) {
  const QPointF c = rect.center();
  const double s = unit * 0.36;
  painter.setPen(QPen(theme::Current().text, std::max(1.0, unit * 0.08)));
  painter.setBrush(Qt::NoBrush);
  if (ps) {
    painter.drawRoundedRect(QRectF(c.x() - s * 0.45, c.y() - s * 0.2, s * 0.9, s * 1.2), s * 0.3, s * 0.3);
    for (const double dx : {-0.6, 0.0, 0.6}) {
      painter.drawLine(QPointF(c.x() + dx * s * 0.75, c.y() - s * 0.55), QPointF(c.x() + dx * s, c.y() - s * 0.95));
    }
    return;
  }
  painter.drawRect(QRectF(c.x() - s * 0.8, c.y() - s * 0.8, s * 1.1, s * 1.1));
  painter.setBrush(QColor("#2a2e36"));
  painter.drawRect(QRectF(c.x() - s * 0.3, c.y() - s * 0.3, s * 1.1, s * 1.1));
}
}  // namespace

double GlyphWidth(double unit, Nav nav, const QString& kind) {
  if (!Wide(nav) && kind != "keys") return unit * 1.45;
  return QFontMetricsF(Font(unit, 0.62, QFont::Bold)).horizontalAdvance(GlyphText(nav, kind)) + unit * 0.9;
}

double DrawGlyph(QPainter& painter, QPointF left_center, double unit, Nav nav, const QString& kind) {
  const QString text = GlyphText(nav, kind);
  nav = PhysicalNav(nav, kind);  // for its color
  // A key is a keycap whatever it is.
  const bool shoulder = Wide(nav) || kind == "keys";
  const double h = unit * 1.45;
  painter.save();
  painter.setFont(Font(unit, shoulder ? 0.62 : 0.78, QFont::Bold));
  const double w = GlyphWidth(unit, nav, kind);
  const QRectF rect(left_center.x(), left_center.y() - h / 2, w, h);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(QColor(255, 255, 255, 60), std::max(1.0, unit * 0.05)));
  painter.setBrush(QColor("#2a2e36"));
  painter.drawRoundedRect(rect, shoulder ? unit * 0.35 : h / 2, shoulder ? unit * 0.35 : h / 2);
  QColor color = theme::Current().text;
  if (kind == "xbox" || kind.isEmpty()) {
    if (nav == Nav::Accept) color = QColor("#6cc04a");
    if (nav == Nav::Back) color = QColor("#ff5c5c");
    if (nav == Nav::Action) color = QColor("#4c9aff");
    if (nav == Nav::Search) color = QColor("#f5c518");
  } else if (kind == "ps") {
    if (nav == Nav::Accept) color = QColor("#8fb0ff");
    if (nav == Nav::Back) color = QColor("#ff6b6b");
    if (nav == Nav::Action) color = QColor("#ee8fdc");
    if (nav == Nav::Search) color = QColor("#4fd1a5");
  }
  if (nav == Nav::Sort && kind != "nin" && kind != "keys") {
    DrawSortIcon(painter, rect, unit, kind == "ps");
  } else {
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignCenter, text);
  }
  painter.restore();
  return w;
}

QRectF DrawTitle(QPainter& painter, const QRectF& box, double unit, double size, const QString& text) {
  for (const double scale : {1.0, 0.82, 0.68}) {
    painter.setFont(Font(unit, size * scale, QFont::ExtraBold));
    const QFontMetricsF metrics(painter.font());
    const QRectF room(box.left(), box.top(), box.width(), metrics.lineSpacing() * 2 + 1);
    const QRectF used = painter.boundingRect(room, Qt::TextWordWrap, text);
    if (used.height() <= room.height() || scale < 0.7) {
      // Past two lines even at the smallest size: cut the end so it wraps into two.
      const QString shown = used.height() <= room.height()
                                ? text
                                : metrics.elidedText(text, Qt::ElideRight, box.width() * 1.85);
      painter.drawText(room, Qt::TextWordWrap, shown);
      return painter.boundingRect(room, Qt::TextWordWrap, shown);
    }
  }
  return {};
}

CoverGrid::CoverGrid(const QRectF& area, int columns, double gap, int focus_row)
    : area(area), columns(columns), gap(gap) {
  tile_w = (area.width() - gap * (columns - 1)) / columns;
  tile_h = std::max(1.0, std::min(tile_w * 1.5, (area.height() - gap) / 2));
  tile_w = tile_h / 1.5;
  left = area.left() + (area.width() - (tile_w * columns + gap * (columns - 1))) / 2;
  const int visible = std::max(1, int((area.height() + gap) / (tile_h + gap)));
  first_row = std::max(0, focus_row - visible + 1);
}

QRectF CoverGrid::Box(int index) const {
  const int row = index / columns - first_row;
  return {left + (index % columns) * (tile_w + gap), area.top() + row * (tile_h + gap), tile_w, tile_h};
}

void DrawGlow(QPainter& painter, const QRectF& tile, double unit) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  QColor glow = Accent();
  // A few widening rings, fainter outward: cheaper than a blur and close enough.
  for (int ring = 4; ring >= 1; --ring) {
    glow.setAlpha(14 + (4 - ring) * 10);
    painter.setBrush(glow);
    const double grow = unit * 0.32 * ring;
    painter.drawRoundedRect(tile.adjusted(-grow, -grow, grow, grow), unit * 0.45 + grow, unit * 0.45 + grow);
  }
  painter.restore();
}

void DrawCover(QPainter& painter, const QRectF& rect, const QPixmap& cover, double unit, bool focused,
               bool dim, double progress) {
  const theme::Tokens& tokens = theme::Current();
  const double radius = unit * 0.45;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  QPainterPath clip;
  clip.addRoundedRect(rect, radius, radius);
  painter.setClipPath(clip);
  if (cover.isNull()) {
    // Not loaded yet: a soft wash of the focus color rather than a flat box.
    QLinearGradient wash(rect.topLeft(), rect.bottomRight());
    QColor tint = Accent();
    tint.setAlpha(46);
    wash.setColorAt(0, tokens.tile_placeholder.lighter(118));
    wash.setColorAt(1, tokens.tile_placeholder);
    painter.fillRect(rect, wash);
    painter.fillRect(rect, tint);
  } else {
    painter.fillRect(rect, tokens.tile_placeholder);
    // Cropped to fill, never stretched: a cover that isn't 2:3 loses its edges.
    const QSizeF shown = QSizeF(rect.size()).scaled(cover.size(), Qt::KeepAspectRatio);
    const QRectF source((cover.width() - shown.width()) / 2, (cover.height() - shown.height()) / 2, shown.width(), shown.height());
    painter.drawPixmap(rect, cover, source);
  }
  if (dim) painter.fillRect(rect, QColor(0, 0, 0, 130));
  // Spelled out, since a darker cover alone doesn't say why.
  if (dim && progress < 0) {
    painter.setFont(Font(unit, 0.72, QFont::Bold));
    const QString label = "↓  Not installed";
    const double w = std::min(rect.width() - unit * 1.2, painter.fontMetrics().horizontalAdvance(label) + unit * 1.4);
    const QRectF pill(rect.center().x() - w / 2, rect.bottom() - unit * 2.1, w, unit * 1.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 190));
    painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    painter.setPen(tokens.text);
    painter.drawText(pill, Qt::AlignCenter, painter.fontMetrics().elidedText(label, Qt::ElideRight, int(w - unit)));
  }
  if (progress >= 0) {
    const QRectF track(rect.left() + unit * 0.6, rect.bottom() - unit * 0.95, rect.width() - unit * 1.2, unit * 0.35);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    painter.setBrush(Accent());
    painter.drawRoundedRect(QRectF(track.topLeft(), QSizeF(track.width() * progress, track.height())),
                            track.height() / 2, track.height() / 2);
  }
  painter.restore();
  if (focused) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    // A soft glow of the accent around the ring.
    QColor glow = Accent();
    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 5; ++i) {
      glow.setAlpha(46 - i * 8);
      const double spread = unit * (0.2 + 0.18 * i);
      painter.setPen(QPen(glow, unit * 0.2));
      painter.drawRoundedRect(rect.adjusted(-spread, -spread, spread, spread), radius + spread, radius + spread);
    }
    const double gap = unit * 0.2;
    painter.setPen(QPen(Accent(), unit * 0.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(-gap, -gap, gap, gap), radius + gap, radius + gap);
    painter.restore();
  }
}

double DrawPill(QPainter& painter, QPointF top_left, double unit, const QString& text, const std::optional<QColor>& dot) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setFont(Font(unit, 0.85, QFont::DemiBold));
  const double pad = unit * 0.6, dot_size = dot ? unit * 0.5 : 0, gap = dot ? unit * 0.4 : 0;
  const double h = unit * 1.5;
  const double w = pad * 2 + dot_size + gap + painter.fontMetrics().horizontalAdvance(text);
  const QRectF rect(top_left, QSizeF(w, h));
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 26));
  painter.drawRoundedRect(rect, h / 2, h / 2);
  if (dot) {
    painter.setBrush(*dot);
    painter.drawEllipse(QRectF(rect.left() + pad, rect.center().y() - dot_size / 2, dot_size, dot_size));
  }
  painter.setPen(theme::Current().text);
  painter.drawText(rect.adjusted(pad + dot_size + gap, 0, -pad, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
  painter.restore();
  return w;
}

QString StatusText(const Item& item, double progress, bool paused) {
  if (paused) return progress >= 0 ? QString("Paused · %1%").arg(qRound(progress * 100)) : QString("Paused");
  if (progress >= 0) return QString("Installing · %1%").arg(qRound(progress * 100));
  if (!item.installed()) return "Not installed";
  if (item.game->running) return "Running";
  return StatusLabel(item.game->status);
}

QColor StatusColor(const Item& item, double progress) {
  const theme::Tokens& tokens = theme::Current();
  if (progress >= 0) return tokens.info;
  if (!item.installed()) return tokens.status_needs_install;
  if (item.game->running) return tokens.running;
  return mira_gui::StatusColor(item.game->status);
}

QString SourceName(const QString& source) {
  if (const SourceInfo* info = FindSourceInfo(source)) return info->name;
  return "Local";  // a scan, a manual add
}

}  // namespace mira_gui::bigscreen
