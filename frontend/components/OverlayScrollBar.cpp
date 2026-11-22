#include "OverlayScrollBar.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QVariantAnimation>
#include <QtMath>
#include <algorithm>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr int kThickness = 4;   // the thumb, hovered or not
constexpr int kInset = 3;       // between the thumb and the area's edge
constexpr int kReach = 12;      // how far in from the edge the thumb can be grabbed
constexpr int kEndGap = 2;      // between the thumb's travel and the area's ends
constexpr int kMinLength = 32;  // the shortest a thumb gets on a long page
constexpr int kLingerMs = 1000;
constexpr const char* kAttached = "overlay_scroll_bar";

}  // namespace

OverlayScrollBar::OverlayScrollBar(QAbstractScrollArea* area, QScrollBar* bar)
    : QWidget(area),
      area_(area),
      bar_(bar),
      linger_(new QTimer(this)),
      fade_(new QVariantAnimation(this)) {
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_TransparentForMouseEvents);  // until it first shows
  setCursor(Qt::ArrowCursor);
  linger_->setSingleShot(true);
  linger_->setInterval(kLingerMs);
  connect(linger_, &QTimer::timeout, this, &OverlayScrollBar::FadeOut);
  fade_->setDuration(150);
  connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    opacity_ = value.toReal();
    update();
  });
  // Faded out it stays in place, so the accessibility tree keeps it, but lets the pointer through.
  connect(fade_, &QVariantAnimation::finished, this, [this] {
    if (opacity_ <= 0) setAttribute(Qt::WA_TransparentForMouseEvents);
  });
  connect(bar, &QScrollBar::valueChanged, this, [this] {
    Place();
    Reveal();
  });
  connect(bar, &QScrollBar::rangeChanged, this, [this] {
    Place();
    setVisible(Scrollable());
  });
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          qOverload<>(&QWidget::update));
  area->installEventFilter(this);
  Place();
  setVisible(Scrollable());
}

bool OverlayScrollBar::Vertical() const {
  return bar_->orientation() == Qt::Vertical;
}

bool OverlayScrollBar::Scrollable() const {
  if (bar_.isNull() || area_.isNull() || bar_->maximum() <= bar_->minimum() || !bar_->isEnabled())
    return false;
  const Qt::ScrollBarPolicy policy =
      Vertical() ? area_->verticalScrollBarPolicy() : area_->horizontalScrollBarPolicy();
  return policy != Qt::ScrollBarAlwaysOff;
}

QRectF OverlayScrollBar::Thumb() const {
  if (!Scrollable()) return {};
  const qreal track = (Vertical() ? height() : width()) - 2 * kEndGap;
  if (track <= 0) return {};
  const qreal range = bar_->maximum() - bar_->minimum();
  // A short area's thumb may be shorter than the minimum, never longer than its travel.
  const qreal length = std::min(
      track, std::max<qreal>(kMinLength, track * bar_->pageStep() / (range + bar_->pageStep())));
  const qreal at = kEndGap + (track - length) * (bar_->sliderPosition() - bar_->minimum()) / range;
  const qreal across = (Vertical() ? width() : height()) - kInset - kThickness;
  return Vertical() ? QRectF(across, at, kThickness, length)
                    : QRectF(at, across, length, kThickness);
}

void OverlayScrollBar::Place() {
  if (area_.isNull()) return;
  const QRect viewport = area_->viewport()->geometry();
  setGeometry(
      Vertical()
          ? QRect(viewport.right() - kReach + 1, viewport.top(), kReach, viewport.height())
          : QRect(viewport.left(), viewport.bottom() - kReach + 1, viewport.width(), kReach));
  // Only the thumb takes the pointer, so content under the rest of the strip stays clickable.
  const QRectF thumb = Thumb();
  if (!thumb.isEmpty()) {
    setMask(Vertical() ? QRect(0, qFloor(thumb.top()), kReach, qCeil(thumb.height()))
                       : QRect(qFloor(thumb.left()), 0, qCeil(thumb.width()), kReach));
  }
  update();
}

void OverlayScrollBar::Reveal() {
  if (!Scrollable() || !area_->isVisible()) return;
  Place();
  show();
  raise();
  setAttribute(Qt::WA_TransparentForMouseEvents, false);
  if (opacity_ < 1 &&
      (fade_->state() != QAbstractAnimation::Running || fade_->endValue().toReal() < 1)) {
    fade_->stop();
    fade_->setStartValue(opacity_);
    fade_->setEndValue(1.0);
    fade_->start();
  }
  if (!hovered_ && drag_offset_ < 0) linger_->start();
}

void OverlayScrollBar::FadeOut() {
  if (hovered_ || drag_offset_ >= 0) return;
  fade_->stop();
  fade_->setStartValue(opacity_);
  fade_->setEndValue(0.0);
  fade_->start();
}

void OverlayScrollBar::paintEvent(QPaintEvent*) {
  const QRectF thumb = Thumb();
  if (thumb.isEmpty() || opacity_ <= 0) return;
  QColor color = theme::Current().text_muted;
  color.setAlphaF((hovered_ || drag_offset_ >= 0 ? 0.95 : 0.6) * opacity_);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawRoundedRect(thumb, kThickness / 2.0, kThickness / 2.0);
}

void OverlayScrollBar::enterEvent(QEnterEvent*) {
  hovered_ = true;
  linger_->stop();
  Reveal();
  update();
}

void OverlayScrollBar::leaveEvent(QEvent*) {
  hovered_ = false;
  if (drag_offset_ < 0) linger_->start();
  update();
}

void OverlayScrollBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !Scrollable()) return;
  const QRectF thumb = Thumb();
  const qreal at = Vertical() ? event->position().y() : event->position().x();
  const qreal start = Vertical() ? thumb.top() : thumb.left();
  drag_offset_ = std::clamp(at - start, qreal(0), Vertical() ? thumb.height() : thumb.width());
  bar_->setSliderDown(true);
  update();
}

void OverlayScrollBar::mouseMoveEvent(QMouseEvent* event) {
  if (drag_offset_ < 0 || !Scrollable()) return;
  const QRectF thumb = Thumb();
  const qreal length = Vertical() ? thumb.height() : thumb.width();
  const qreal track = (Vertical() ? height() : width()) - 2 * kEndGap - length;
  if (track <= 0) return;
  const qreal at = (Vertical() ? event->position().y() : event->position().x()) - drag_offset_;
  const qreal fraction = std::clamp((at - kEndGap) / track, qreal(0), qreal(1));
  bar_->setSliderPosition(qRound(bar_->minimum() + fraction * (bar_->maximum() - bar_->minimum())));
}

void OverlayScrollBar::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || drag_offset_ < 0) return;
  drag_offset_ = -1;
  if (!bar_.isNull()) bar_->setSliderDown(false);
  if (!hovered_) linger_->start();
  update();
}

void OverlayScrollBar::wheelEvent(QWheelEvent* event) {
  // The bar is where the app's smooth wheel works.
  if (!bar_.isNull()) QCoreApplication::sendEvent(bar_, event);
}

bool OverlayScrollBar::eventFilter(QObject* watched, QEvent* event) {
  if (watched == area_) {
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::LayoutRequest:
      case QEvent::Show:
        Place();
        setVisible(Scrollable());
        break;
      case QEvent::Hide:
        fade_->stop();
        opacity_ = 0;
        setAttribute(Qt::WA_TransparentForMouseEvents);
        break;
      default:
        break;
    }
  }
  return false;
}

bool OverlayScrollBar::Shown() const {
  return !testAttribute(Qt::WA_TransparentForMouseEvents);
}

namespace {

// Watches for each scroll bar's first polish, when the area first shows it.
class Installer : public QObject {
 public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::Polish) return false;
    auto* bar = qobject_cast<QScrollBar*>(watched);
    if (bar == nullptr || bar->property(kAttached).toBool()) return false;
    // A bar's parent is the area's container for it.
    QWidget* container = bar->parentWidget();
    auto* area = qobject_cast<QAbstractScrollArea*>(container != nullptr ? container->parentWidget()
                                                                         : nullptr);
    if (area == nullptr) return false;
    if (bar != area->verticalScrollBar() && bar != area->horizontalScrollBar()) return false;
    bar->setProperty(kAttached, true);
    new OverlayScrollBar(area, bar);
    return false;
  }
};

}  // namespace

void SetUpOverlayScrollBars() {
  qApp->installEventFilter(new Installer(qApp));
}

}  // namespace mira_gui
