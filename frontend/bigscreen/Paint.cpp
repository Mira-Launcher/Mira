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

QString GlyphText(Nav nav, const QString& kind) {
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
  return nav == Nav::PrevTab || nav == Nav::NextTab || nav == Nav::Sort || nav == Nav::PrevLetter || nav == Nav::NextLetter;
}
}  // namespace

double GlyphWidth(double unit, Nav nav, const QString& kind) {
  if (!Wide(nav)) return unit * 1.45;
  return QFontMetricsF(Font(unit, 0.62, QFont::Bold)).horizontalAdvance(GlyphText(nav, kind)) + unit * 0.9;
}

double DrawGlyph(QPainter& painter, QPointF left_center, double unit, Nav nav, const QString& kind) {
  const QString text = GlyphText(nav, kind);
  const bool shoulder = Wide(nav);
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
  painter.setPen(color);
  painter.drawText(rect, Qt::AlignCenter, text);
  painter.restore();
  return w;
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
  painter.fillRect(rect, tokens.tile_placeholder);
  if (!cover.isNull()) painter.drawPixmap(rect, cover, QRectF(cover.rect()));
  if (dim) painter.fillRect(rect, QColor(0, 0, 0, 110));
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
