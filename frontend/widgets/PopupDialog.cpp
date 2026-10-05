#include "PopupDialog.h"

#include "../theme/Icons.h"
#include "../theme/Theme.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace mira_gui {
namespace {

constexpr int kWidth = 380;
constexpr int kMargin = 20;
constexpr int kStripe = 4;

}  // namespace

PopupDialog::PopupDialog(QWidget* parent, notify::Level level, const QString& title)
    : QDialog(parent), level_(level) {
  // Frameless: no WM title bar. Translucent: paintEvent's rounded card
  // shows through the square corners instead of a solid one behind it.
  setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setModal(true);
  setFixedWidth(kWidth);

  auto* outer = new QVBoxLayout(this);
  // Left inset wider: room for paintEvent's accent stripe, same language as
  // ToastCard's border-left.
  outer->setContentsMargins(kMargin + kStripe, kMargin, kMargin, kMargin);
  outer->setSpacing(14);

  auto* heading_row = new QHBoxLayout();
  auto* heading = new QLabel(title, this);
  heading->setProperty("role", "heading");
  heading->setWordWrap(true);
  heading_row->addWidget(heading, /*stretch=*/1);

  // Top-right X: the cancel/dismiss affordance a frameless dialog has no WM
  // titlebar to supply one for. Same hand-drawn glyph as the top bar's own
  // window controls, not a font character: nothing guarantees a fallback
  // font has one.
  close_button_ = new QToolButton(this);
  close_button_->setAutoRaise(true);
  close_button_->setIcon(icons::For(icons::Glyph::Close));
  close_button_->setCursor(Qt::PointingHandCursor);
  connect(close_button_, &QToolButton::clicked, this, &QDialog::reject);
  heading_row->addWidget(close_button_, 0, Qt::AlignTop);
  outer->addLayout(heading_row);

  body_ = new QVBoxLayout();
  body_->setSpacing(8);
  message_ = new QLabel(this);
  message_->setWordWrap(true);
  message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  body_->addWidget(message_);
  outer->addLayout(body_);

  buttons_ = new QHBoxLayout();
  buttons_->addStretch(1);
  outer->addLayout(buttons_);
}

void PopupDialog::SetMessage(const QString& text) { message_->setText(text); }

void PopupDialog::SetDetail(const QString& text) {
  if (detail_ == nullptr) {
    detail_ = new QLabel(this);
    detail_->setWordWrap(true);
    detail_->setProperty("role", "muted");
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body_->addWidget(detail_);
  }
  detail_->setText(text);
}

void PopupDialog::SetAction(const QString& label, const QString& url) {
  SetAction(label, [url] { QDesktopServices::openUrl(QUrl(url)); });
}

void PopupDialog::SetAction(const QString& label, std::function<void()> activated) {
  if (action_ == nullptr) {
    action_ = new QLabel(this);
    action_->setTextFormat(Qt::RichText);
    action_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    action_->setOpenExternalLinks(false);
    body_->addWidget(action_);
  }
  action_->setText(QString("<a href=\"#\">%1</a>").arg(label.toHtmlEscaped()));
  connect(action_, &QLabel::linkActivated, this, [this, activated = std::move(activated)] {
    activated();
    accept();
  });
}

QPushButton* PopupDialog::AddButton(const QString& text, bool accept_role, bool default_button) {
  return AddButton(text, accept_role ? QDialog::Accepted : QDialog::Rejected, default_button);
}

QPushButton* PopupDialog::AddButton(const QString& text, int result, bool default_button) {
  auto* button = new QPushButton(text, this);
  button->setAutoDefault(default_button);
  button->setDefault(default_button);
  connect(button, &QPushButton::clicked, this, [this, result] { done(result); });
  // Before the trailing stretch, so buttons stack left-to-right.
  buttons_->insertWidget(buttons_->count() - 1, button);
  return button;
}

void PopupDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  // WM placement heuristics are tuned for decorated windows and place a
  // frameless one poorly. Center over the parent, matching QMessageBox.
  if (QWidget* owner = parentWidget() != nullptr ? parentWidget()->window() : nullptr) {
    move(owner->geometry().center() - rect().center());
  } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
    move(screen->availableGeometry().center() - rect().center());
  }
}

void PopupDialog::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  QPainterPath card;
  card.addRoundedRect(rect(), tokens.radius_panel, tokens.radius_panel);
  painter.fillPath(card, tokens.surface);
  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(card);

  QPainterPath stripe;
  stripe.addRoundedRect(QRectF(0, 0, kStripe * 2, height()), tokens.radius_panel,
                        tokens.radius_panel);
  painter.setClipPath(card);
  painter.fillPath(stripe, notify::AccentFor(level_));
}

}  // namespace mira_gui
