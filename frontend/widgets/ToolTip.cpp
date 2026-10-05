#include "ToolTip.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScreen>

#include <algorithm>
#include <optional>

#include "../library/HoverCard.h"
#include "../theme/Theme.h"

namespace mira_gui::tooltip {
namespace {

// Wider than this wraps.
constexpr int kMaxWidth = 320;

// Set by AlignLeftWith: the widget whose left side the tooltip starts at.
constexpr const char* kAlignProperty = "mira_tooltip_align";
// In from that side, so the two edges don't read as one line, and a little
// closer to the label than a plain tooltip.
constexpr int kAlignInset = 6;
constexpr int kAlignDrop = -2;

class TipCard : public QWidget {
public:
  TipCard() : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  // Drawn, not a QLabel: a word-wrapped QLabel's heightForWidth can run a
  // line or more past what it paints, leaving empty bands in the card.
  // `left`, when set, is the global x the card starts at, rather than centered under `anchor`.
  void ShowText(const QString& text, const QRect& anchor, bool beside, std::optional<int> left) {
    text_ = text;
    ensurePolished();
    const QRect text_rect =
        fontMetrics().boundingRect(QRect(0, 0, kMaxWidth, 100000), Qt::TextWordWrap, text_);
    text_size_ = text_rect.size();
    setFixedSize(text_size_.width() + 2 * card::kPaddingX, text_size_.height() + 2 * kPaddingY);
    QPoint pos = card::Place(anchor, size(), beside);
    if (left) {
      // Still kept on screen at the right.
      const QScreen* screen = QGuiApplication::screenAt(anchor.center());
      const int right = screen != nullptr ? screen->availableGeometry().right() : *left + width();
      pos.setX(std::min(*left, right + 1 - width()));
      // Away from the label, whichever side of it Place put the card.
      pos.ry() += pos.y() > anchor.top() ? kAlignDrop : -kAlignDrop;
    }
    move(pos);
    update();
    show();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    card::Paint(this);
    QPainter painter(this);
    painter.setPen(theme::Current().text);
    painter.drawText(QRect(QPoint(card::kPaddingX, kPaddingY), text_size_), Qt::TextWordWrap, text_);
  }

private:
  // A line of text needs less than a card's worth of padding.
  static constexpr int kPaddingY = card::kPaddingY - 3;
  QString text_;
  QSize text_size_;
};

// QAction::toolTip() falls back to the action's text; only a tooltip that
// says something more is worth showing.
QString ExplicitToolTip(const QAction* action) {
  auto stripped = [](QString text) {
    text.remove('&');
    text.remove("...");
    text.remove(QChar(0x2026));
    return text.trimmed();
  };
  const QString tip = action->toolTip();
  return stripped(tip) == stripped(action->text()) ? QString() : tip;
}

QRect GlobalRect(const QWidget* widget, const QRect& local) {
  return QRect(widget->mapToGlobal(local.topLeft()), local.size());
}

class ToolTipFilter : public QObject {
public:
  explicit ToolTipFilter(QObject* parent) : QObject(parent) {
    // Deleted before QApplication is, like any other top-level widget.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
      delete card_;
      card_ = nullptr;
    });
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    switch (event->type()) {
      case QEvent::ToolTip:
        return ShowFor(watched, static_cast<QHelpEvent*>(event));
      case QEvent::Leave:
      case QEvent::Hide:
        if (watched == target_) Hide();
        break;
      case QEvent::MouseMove:
        if (card_ != nullptr && card_->isVisible() && !anchor_.contains(QCursor::pos())) Hide();
        break;
      case QEvent::MouseButtonPress:
      case QEvent::Wheel:
      case QEvent::KeyPress:
      case QEvent::WindowDeactivate:
        Hide();
        break;
      default:
        break;
    }
    return false;
  }

private:
  bool ShowFor(QObject* watched, QHelpEvent* help) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;

    QString text;
    QRect anchor;
    bool beside = false;
    std::optional<int> left;
    if (auto* menu = qobject_cast<QMenu*>(widget)) {
      QAction* action = menu->actionAt(help->pos());
      if (action != nullptr) {
        text = ExplicitToolTip(action);
        // The whole row's height, beside the menu rather than over the next item.
        const QRect row = menu->actionGeometry(action);
        anchor = GlobalRect(menu, QRect(0, row.top(), menu->width(), row.height()));
        beside = true;
      }
    } else if (auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget());
               view != nullptr && view->viewport() == widget) {
      const QModelIndex index = view->indexAt(help->pos());
      text = index.data(Qt::ToolTipRole).toString();
      anchor = GlobalRect(widget, view->visualRect(index));
    } else {
      text = widget->toolTip();
      // Qt passes it on to the parent, which may have one.
      if (text.isEmpty()) return false;
      anchor = GlobalRect(widget, widget->rect());
      if (auto* edge = qobject_cast<QWidget*>(widget->property(kAlignProperty).value<QObject*>())) {
        left = edge->mapToGlobal(QPoint(kAlignInset, 0)).x();
      }
    }

    if (text.isEmpty()) {
      Hide();
      return true;
    }
    if (card_ == nullptr) card_ = new TipCard();
    target_ = widget;
    anchor_ = anchor;
    card_->ShowText(text, anchor, beside, left);
    return true;
  }

  void Hide() {
    if (card_ != nullptr) card_->hide();
    target_ = nullptr;
  }

  TipCard* card_ = nullptr;
  QPointer<QWidget> target_;
  QRect anchor_;
};

}  // namespace

void Install() { qApp->installEventFilter(new ToolTipFilter(qApp)); }

void AlignLeftWith(QWidget* widget, QWidget* edge) {
  widget->setProperty(kAlignProperty, QVariant::fromValue<QObject*>(edge));
}

}  // namespace mira_gui::tooltip
