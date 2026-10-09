#include "BrowserSketch.h"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr int kBar = 28;  // the browser's address bar, or the app window's title bar
constexpr int kPad = 12;  // around the page
constexpr int kGap = 8;   // between parts
constexpr int kRadius = 8;

QColor Mix(const QColor& a, const QColor& b, double t) {
  return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                          a.greenF() + (b.greenF() - a.greenF()) * t,
                          a.blueF() + (b.blueF() - a.blueF()) * t);
}

// The accent outline and wash that says "this one".
void Mark(QPainter* painter, const QRectF& rect) {
  const theme::Tokens& tokens = theme::Current();
  QColor wash = tokens.accent;
  wash.setAlphaF(0.16);
  painter->setPen(QPen(tokens.accent, 1.5));
  painter->setBrush(wash);
  painter->drawRoundedRect(rect.adjusted(-3, -2, 3, 2), 4, 4);
}

void StepBadge(QPainter* painter, const QPointF& center, int step) {
  const theme::Tokens& tokens = theme::Current();
  painter->setPen(Qt::NoPen);
  painter->setBrush(tokens.accent);
  painter->drawEllipse(center, 8, 8);
  QFont font = painter->font();
  font.setPixelSize(10);
  font.setBold(true);
  painter->setFont(font);
  painter->setPen(tokens.on_accent);
  painter->drawText(QRectF(center.x() - 8, center.y() - 8, 16, 16), Qt::AlignCenter,
                    QString::number(step));
}

}  // namespace

BrowserSketch::BrowserSketch(QWidget* parent) : QWidget(parent) {
  QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);
}

void BrowserSketch::SetSketch(const Sketch& sketch) {
  sketch_ = sketch;
  updateGeometry();
  update();
}

int BrowserSketch::heightForWidth(int width) const {
  return Layout(width, nullptr);
}

QSize BrowserSketch::sizeHint() const {
  return {360, heightForWidth(360)};
}

QSize BrowserSketch::minimumSizeHint() const {
  return {240, heightForWidth(240)};
}

void BrowserSketch::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  Layout(width(), &painter);
}

int BrowserSketch::Layout(int width, QPainter* painter) const {
  const theme::Tokens& tokens = theme::Current();
  const QColor page = Mix(tokens.surface, tokens.window, 0.5);
  QFont base = font();
  base.setPixelSize(12);
  QFont small = base;
  small.setPixelSize(11);
  QFont heading = base;
  heading.setPixelSize(14);
  heading.setBold(true);
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(11);

  const int inner = width - 2 * kPad;
  int y = kBar + kPad;
  const auto text_height = [](const QFont& font, int w, const QString& text, int flags) {
    return QFontMetrics(font).boundingRect(QRect(0, 0, w, 100000), flags, text).height();
  };

  // Parts first, so the frame knows its height; with a painter they're drawn on the page after it.
  struct Placed {
    const SketchPart* part;
    QRect rect;
  };
  std::vector<Placed> placed;
  for (const SketchPart& part : sketch_.parts) {
    const int indent = part.step > 0 ? 22 : 0;  // room for the badge
    const int w = inner - indent;
    const QString text = part.text.value(0);
    int h = 0;
    int item_w = w;
    switch (part.kind) {
      case SketchPart::Kind::Heading:
        h = text_height(heading, w, text, Qt::TextWordWrap);
        break;
      case SketchPart::Kind::Text:
        h = text_height(small, w, text, Qt::TextWordWrap);
        break;
      case SketchPart::Kind::Code:
        h = text_height(mono, w, text, Qt::TextWrapAnywhere);
        break;
      case SketchPart::Kind::Button:
      case SketchPart::Kind::Field:
        h = 24;
        item_w = std::min(w, QFontMetrics(base).horizontalAdvance(text) + 24);
        if (part.kind == SketchPart::Kind::Field) item_w = std::max(item_w, std::min(w, 180));
        break;
      case SketchPart::Kind::Blank:
        h = 64;
        break;
      case SketchPart::Kind::Tabs:
      case SketchPart::Kind::TableRow:
        h = 20;
        break;
    }
    placed.push_back({&part, QRect(kPad + indent, y, item_w, h)});
    y += h + (part.kind == SketchPart::Kind::TableRow ? 2 : kGap);
  }
  const int height = y - kGap + kPad;
  if (painter == nullptr) return height;

  // The frame: a window with its bar, then the page.
  const QRectF frame(0.5, 0.5, width - 1, height - 1);
  QPainterPath outline;
  outline.addRoundedRect(frame, kRadius, kRadius);
  painter->setPen(QPen(tokens.border, 1));
  painter->setBrush(page);
  painter->drawPath(outline);
  painter->save();
  painter->setClipPath(outline);
  painter->fillRect(QRectF(0, 0, width, kBar), tokens.surface_alt);
  painter->restore();
  painter->setPen(QPen(tokens.border, 1));
  painter->drawLine(QPointF(0.5, kBar + 0.5), QPointF(width - 0.5, kBar + 0.5));

  if (sketch_.address.isEmpty()) {
    painter->setFont(base);
    painter->setPen(tokens.text);
    painter->drawText(QRect(kPad, 0, inner, kBar), Qt::AlignVCenter | Qt::AlignLeft, sketch_.title);
  } else {
    // Three window dots, then the address in its pill.
    painter->setPen(Qt::NoPen);
    painter->setBrush(tokens.border);
    for (int i = 0; i < 3; ++i)
      painter->drawEllipse(QPointF(kPad + 4 + i * 11, kBar / 2.0), 3.5, 3.5);
    const QRect pill(kPad + 40, 5, width - kPad * 2 - 40, kBar - 10);
    painter->setBrush(page);
    painter->drawRoundedRect(pill, 4, 4);
    painter->setFont(small);
    const QString address =
        QFontMetrics(small).elidedText(sketch_.address, Qt::ElideRight, pill.width() - 12);
    if (sketch_.address_marked) Mark(painter, pill.adjusted(3, 2, -3, -2));
    painter->setPen(tokens.text);
    painter->drawText(pill.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, address);
  }

  for (const Placed& item : placed) {
    const SketchPart& part = *item.part;
    const QRect& r = item.rect;
    const QString text = part.text.value(0);
    if (part.marked && part.kind != SketchPart::Kind::TableRow) Mark(painter, r);
    switch (part.kind) {
      case SketchPart::Kind::Heading:
        painter->setFont(heading);
        painter->setPen(tokens.text);
        painter->drawText(r, Qt::TextWordWrap, text);
        break;
      case SketchPart::Kind::Text:
        painter->setFont(small);
        painter->setPen(tokens.text_muted);
        painter->drawText(r, Qt::TextWordWrap, text);
        break;
      case SketchPart::Kind::Code:
        painter->setFont(mono);
        painter->setPen(tokens.text);
        painter->drawText(r, Qt::TextWrapAnywhere, text);
        break;
      case SketchPart::Kind::Button:
        painter->setPen(Qt::NoPen);
        painter->setBrush(part.marked ? Qt::transparent : QBrush(tokens.surface_alt));
        painter->drawRoundedRect(r, 4, 4);
        painter->setFont(base);
        painter->setPen(tokens.text);
        painter->drawText(r, Qt::AlignCenter, text);
        break;
      case SketchPart::Kind::Field:
        painter->setPen(QPen(tokens.border, 1));
        painter->setBrush(Qt::NoBrush);
        if (!part.marked) painter->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter->setFont(base);
        painter->setPen(part.marked ? tokens.text : tokens.text_muted);
        painter->drawText(r.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        break;
      case SketchPart::Kind::Blank:
        painter->setFont(small);
        painter->setPen(tokens.text_muted);
        painter->drawText(r, Qt::AlignCenter, text);
        break;
      case SketchPart::Kind::Tabs: {
        painter->setFont(small);
        int x = r.left();
        for (int i = 0; i < part.text.size(); ++i) {
          const int w = QFontMetrics(small).horizontalAdvance(part.text[i]);
          if (x + w > r.right()) break;
          painter->setPen(i == part.selected ? tokens.text : tokens.text_muted);
          painter->drawText(QRect(x, r.top(), w, r.height()), Qt::AlignVCenter, part.text[i]);
          if (i == part.selected) painter->fillRect(QRect(x, r.bottom() - 1, w, 2), tokens.accent);
          x += w + 12;
        }
        break;
      }
      case SketchPart::Kind::TableRow: {
        if (part.marked) Mark(painter, r);
        painter->setFont(mono);
        painter->setPen(tokens.text);
        const int name_w = r.width() * 2 / 5;
        const QFontMetrics metrics(mono);
        painter->drawText(QRect(r.left() + 4, r.top(), name_w - 8, r.height()), Qt::AlignVCenter,
                          metrics.elidedText(part.text.value(0), Qt::ElideRight, name_w - 8));
        painter->setPen(part.marked ? tokens.text : tokens.text_muted);
        const int value_w = r.width() - name_w - 4;
        painter->drawText(QRect(r.left() + name_w, r.top(), value_w, r.height()), Qt::AlignVCenter,
                          metrics.elidedText(part.text.value(1), Qt::ElideRight, value_w));
        break;
      }
    }
    if (part.step > 0) StepBadge(painter, QPointF(kPad + 8, r.center().y() + 0.5), part.step);
  }
  return height;
}

}  // namespace mira_gui
