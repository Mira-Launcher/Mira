#include "PopupDialog.h"

#include <QDesktopServices>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "Button.h"
#include "ModalOverlay.h"
#include "SheetCard.h"
#include "Text.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 420;
constexpr int kIcon = 22;

std::pair<icons::Glyph, icons::Role> ToneIcon(Tone tone) {
  switch (tone) {
    case Tone::Success:
      return {icons::Glyph::CheckCircle, &theme::Tokens::success};
    case Tone::Warning:
      return {icons::Glyph::Warning, &theme::Tokens::warning};
    case Tone::Danger:
      return {icons::Glyph::CircleX, &theme::Tokens::error};
    default:
      break;
  }
  return {icons::Glyph::Info, &theme::Tokens::info};
}

// Shown over its window only while that window is up; from the tray it needs a window of its own.
QWidget* HostWindow(QWidget* parent) {
  QWidget* window = parent != nullptr ? parent->window() : nullptr;
  return window != nullptr && window->isVisible() && !window->isMinimized() ? window : nullptr;
}

}  // namespace

PopupDialog::PopupDialog(QWidget* parent, Tone tone, const QString& title)
    : QDialog(HostWindow(parent) != nullptr ? HostWindow(parent) : parent) {
  QWidget* host = HostWindow(parent);
  overlay_ = host != nullptr;
  setModal(true);
  if (overlay_) {
    // A child laid over the whole window rather than a window of its own, so no window manager
    // places, decorates or shadows it.
    setWindowFlags(Qt::Widget);
    setGeometry(host->rect());
    host->installEventFilter(this);
  } else {
    // Translucent: the card's rounded corners show through instead of a square behind them.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
  }

  auto* outer = new QGridLayout(this);
  outer->setContentsMargins(overlay_ ? QMargins(24, 24, 24, 24) : QMargins());
  card_ = new SheetCard(QString(), this);
  card_->setFixedWidth(kWidth);
  outer->addWidget(card_, 0, 0, Qt::AlignCenter);

  auto* icon = new QLabel(card_);
  const auto [glyph, role] = ToneIcon(tone);
  icons::Follow(icon, glyph, kIcon, role);
  card_->SetLeading(icon);
  card_->SetHeading(title);
  card_->SetClose("Close", [this] { reject(); });

  // Lined up under the title, past the icon.
  auto* body = new QWidget(card_);
  body_ = new QVBoxLayout(body);
  body_->setContentsMargins(18 + kIcon + 10, 0, 18, 6);
  body_->setSpacing(6);
  message_ = new QLabel(body);
  message_->setWordWrap(true);
  message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  message_->hide();
  body_->addWidget(message_);
  card_->AddRow(body);

  buttons_ = card_->ButtonRow();
}

void PopupDialog::SetMessage(const QString& text) {
  message_->setText(text);
  message_->setVisible(!text.isEmpty());
}

void PopupDialog::SetDetail(const QString& text) {
  if (detail_ == nullptr) {
    detail_ = new QLabel(this);
    detail_->setWordWrap(true);
    SetTextStyle(detail_, text::kDetail);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body_->addWidget(detail_);
  }
  detail_->setText(text);
}

void PopupDialog::AddWidget(QWidget* widget) {
  body_->addWidget(widget);
}

void PopupDialog::SetWidth(int width) {
  card_->setFixedWidth(width);
}

void PopupDialog::SetAction(const QString& label, const QString& url) {
  SetAction(label, [url] { QDesktopServices::openUrl(QUrl(url)); });
}

void PopupDialog::SetAction(const QString& label, std::function<void()> activated) {
  if (action_ == nullptr) {
    action_ = new Button({}, ButtonKind::Link, this);
    body_->addWidget(action_, 0, Qt::AlignLeft);
  }
  action_->setText(label);
  action_->disconnect(this);  // a second SetAction replaces the first
  connect(action_, &QPushButton::clicked, this, [this, activated = std::move(activated)] {
    activated();
    accept();
  });
}

Button* PopupDialog::AddButton(const QString& text, bool accept_role, bool default_button) {
  return AddButton(text, accept_role ? QDialog::Accepted : QDialog::Rejected, default_button);
}

Button* PopupDialog::AddButton(const QString& text, int result, bool default_button) {
  // The answer Enter gives looks like the action only when it is one: a default Cancel stays plain.
  auto* button = new Button(
      text,
      default_button && result != QDialog::Rejected ? ButtonKind::Primary : ButtonKind::Secondary,
      this);
  button->setAutoDefault(default_button);
  button->setDefault(default_button);
  connect(button, &QPushButton::clicked, this, [this, result] { done(result); });
  // After the stretch: rejecting buttons lead, the default one ends the row.
  int at = buttons_->count();
  if (result == QDialog::Rejected) {
    at = 1;
  } else if (last_ != nullptr) {
    at = buttons_->indexOf(last_);
  }
  buttons_->insertWidget(at, button);
  if (default_button) default_ = button;
  if (default_button && result != QDialog::Rejected) last_ = button;
  return button;
}

void PopupDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  if (overlay_) {
    raise();
    if (default_ != nullptr) default_->setFocus();
    return;
  }
  // Tall enough for the wrapped lines at this width; a dialog's own size hint ignores wrapping.
  if (layout()->hasHeightForWidth()) setFixedHeight(layout()->totalHeightForWidth(width()));
  // WM placement heuristics are tuned for decorated windows and place a
  // frameless one poorly. Center over the parent, matching QMessageBox.
  if (QWidget* owner = parentWidget() != nullptr ? parentWidget()->window() : nullptr) {
    move(owner->geometry().center() - rect().center());
  } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
    move(screen->availableGeometry().center() - rect().center());
  }
}

void PopupDialog::paintEvent(QPaintEvent*) {
  if (!overlay_) return;
  QPainter painter(this);
  painter.fillRect(rect(), ModalScrim());
}

bool PopupDialog::eventFilter(QObject* watched, QEvent* event) {
  if (overlay_ && watched == parentWidget() && event->type() == QEvent::Resize)
    setGeometry(parentWidget()->rect());
  return QDialog::eventFilter(watched, event);
}

// A click beside the card is a no, as a click outside any other modal is.
void PopupDialog::mousePressEvent(QMouseEvent* event) {
  if (overlay_ && event->button() == Qt::LeftButton &&
      !card_->geometry().contains(event->position().toPoint())) {
    reject();
    return;
  }
  QDialog::mousePressEvent(event);
}

}  // namespace mira_gui
