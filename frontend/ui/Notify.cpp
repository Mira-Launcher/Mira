#include "Notify.h"

#include "PopupDialog.h"
#include "SystemNotifier.h"
#include "Theme.h"

#include <QEvent>

#include <algorithm>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QShowEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace mira_gui::notify {
namespace {

using system_notifier::Urgency;

// Fallback card only: how long a transient one stays. A persistent one
// stays until clicked.
constexpr int kTransientCardSeconds = 6;

constexpr int kMargin = 16;
// Fixed, so a one-line toast and a three-line one are the same shape and
// the stack doesn't reflow as messages come and go.
constexpr int kCardWidth = 320;
constexpr int kCardPadding = 12;
constexpr int kCardSpacing = 8;
constexpr int kTextWidth = kCardWidth - 2 * kCardPadding;
constexpr int kFontPixelSize = 12;
// Higher than a timed toast needs, since the default is untimed and
// pushing a card out means discarding something unread. Still bounded.
constexpr int kMaxVisible = 6;
constexpr const char* kHostName = "mira_toast_host";

// One card. Click anywhere on it to dismiss.
//
// Not a QObject subclass: overriding mousePressEvent needs no signals, and
// staying out of moc's way avoids a generated-include dance for no gain.
class ToastCard : public QFrame {
public:
  ToastCard(Level level, const QString& text, QWidget* parent) : QFrame(parent) {
    setCursor(Qt::PointingHandCursor);
    const theme::Tokens& tokens = theme::Current();
    setStyleSheet(QString("QFrame { background: %1; border-radius: %2px; "
                          "border-left: 4px solid %3; }"
                          "QLabel { color: %4; }")
                      .arg(tokens.surface_alt.name())
                      .arg(tokens.radius_toast)
                      .arg(AccentFor(level).name())
                      .arg(tokens.text.name()));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    auto* label = new QLabel(text, this);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    // Set here, not in the stylesheet: height is measured from this font
    // below, and a stylesheet is applied too late to be measured.
    QFont font = label->font();
    font.setPixelSize(kFontPixelSize);
    label->setFont(font);
    layout->addWidget(label);

    label_ = label;
    label->setFixedWidth(kTextWidth);
    setFixedWidth(kCardWidth);
  }

  // Settled here, not in the constructor: a word-wrapped QLabel can't say
  // how tall it is until its final font is polished onto it, which happens
  // on the way to being shown. Measuring early clipped the second line.
  void showEvent(QShowEvent* event) override {
    QFrame::showEvent(event);
    const int measured = label_->heightForWidth(kTextWidth);
    const int text_height = measured > 0 ? measured : label_->sizeHint().height();
    label_->setFixedHeight(text_height);
    setFixedHeight(text_height + 2 * kCardPadding);
  }

  void Dismiss() {
    if (dismissing_) return;
    dismissing_ = true;

    auto* fade = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(fade);
    auto* animation = new QPropertyAnimation(fade, "opacity", this);
    animation->setDuration(220);
    animation->setStartValue(1.0);
    animation->setEndValue(0.0);
    connect(animation, &QPropertyAnimation::finished, this, &QObject::deleteLater);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
  }

protected:
  void mousePressEvent(QMouseEvent* event) override {
    Dismiss();
    event->accept();
  }

private:
  QLabel* label_ = nullptr;
  bool dismissing_ = false;
};

// The stack the cards live in: a plain child widget pinned to the bottom
// right of the window, resized to its contents.
//
// A child, not a separate top-level: a floating notification window would
// be a second taskbar entry, wouldn't follow its owning window, and on
// Wayland couldn't reliably position itself at all.
class ToastHost : public QWidget {
public:
  explicit ToastHost(QWidget* window) : QWidget(window) {
    setObjectName(kHostName);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kCardSpacing);
    setFixedWidth(kCardWidth);
    window->installEventFilter(this);
  }

  void Add(Level level, const QString& text, bool persistent) {
    auto* card = new ToastCard(level, text, this);
    layout()->addWidget(card);

    // Oldest first, so a burst of events can't push the window off screen.
    while (layout()->count() > kMaxVisible) {
      // dynamic_cast, not qobject_cast: these cards carry no Q_OBJECT, so
      // there's no meta-object for qobject_cast to consult.
      if (auto* oldest = dynamic_cast<ToastCard*>(layout()->itemAt(0)->widget())) {
        oldest->Dismiss();
        layout()->removeWidget(oldest);
      } else {
        break;
      }
    }

    // Shown before Reposition, so its showEvent has settled its height by
    // the time the stack is measured.
    card->show();

    if (!persistent) {
      QTimer::singleShot(kTransientCardSeconds * 1000, card, [card] { card->Dismiss(); });
    }
    // The card's own destruction shrinks the stack, so the host must follow
    // it back down.
    connect(card, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, [this] {
                                                       Reposition();
                                                     }); });
    show();
    raise();
    Reposition();
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == parentWidget() && event->type() == QEvent::Resize) Reposition();
    return QWidget::eventFilter(watched, event);
  }

private:
  void Reposition() {
    QWidget* window = parentWidget();
    if (window == nullptr) return;
    // Summed, not taken from layout()->sizeHint(), which kept answering
    // with one card's worth regardless of how many were in it.
    int stack = 0;
    for (int i = 0; i < layout()->count(); ++i) {
      if (QWidget* card = layout()->itemAt(i)->widget()) stack += card->height() + kCardSpacing;
    }
    resize(kCardWidth, qMax(0, stack - kCardSpacing));
    move(window->width() - width() - kMargin, window->height() - height() - kMargin);
    raise();
  }
};

ToastHost* HostFor(QWidget* parent) {
  if (parent == nullptr) return nullptr;
  QWidget* window = parent->window();
  if (window == nullptr) return nullptr;
  if (QWidget* existing = window->findChild<QWidget*>(kHostName, Qt::FindDirectChildrenOnly)) {
    return static_cast<ToastHost*>(existing);
  }
  return new ToastHost(window);
}

QString JoinDetail(const QString& detail, const QString& hint) {
  if (detail.isEmpty()) return hint;
  if (hint.isEmpty()) return detail;
  return detail + "\n\n" + hint;
}

// Fallback only: a modal popup, as every failure used to be.
void FailedPopup(QWidget* parent, const QString& what, const QString& detail, const QString& action,
                 std::function<void()> activate) {
  PopupDialog dialog(parent, Level::Error, "Mira");
  dialog.SetMessage(what);
  if (!detail.isEmpty()) dialog.SetDetail(detail);
  if (!action.isEmpty() && activate) dialog.SetAction(action, std::move(activate));
  dialog.AddButton("OK", /*accept_role=*/true, /*default_button=*/true);
  dialog.exec();
}

void ShowCard(QWidget* parent, Level level, const QString& text, bool persistent) {
  if (ToastHost* host = HostFor(parent)) host->Add(level, text, persistent);
}

}  // namespace

void Failed(QWidget* parent, const QString& what, const QString& detail) {
  FailedWithAction(parent, what, detail, QString(), QString(), {});
}

void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint) {
  FailedWithAction(parent, what, detail, hint, QString(), {});
}

namespace {

// The click lands with Mira possibly hidden in the tray or behind other
// windows, so bring it forward before routing anywhere inside it.
std::function<void()> RaiseThen(QWidget* parent, std::function<void()> activate) {
  QPointer<QWidget> window = parent != nullptr ? parent->window() : nullptr;
  return [window, activate] {
    if (window) {
      window->show();
      window->raise();
      window->activateWindow();
    }
    activate();
  };
}

}  // namespace

void FailedWithAction(QWidget* parent, const QString& what, const QString& detail,
                      const QString& hint, const QString& action,
                      std::function<void()> activate) {
  const QString body = JoinDetail(detail, hint);
  const std::function<void()> on_action = activate ? RaiseThen(parent, activate) : std::function<void()>();
  if (system_notifier::Send(Urgency::Persistent, what, body, action, on_action)) return;
  FailedPopup(parent, what, body, action, std::move(activate));
}

bool AskOutOfSight(QWidget* parent, const QString& title, const QString& body, const QString& action,
                   std::function<void()> activate) {
  return system_notifier::Send(Urgency::Persistent, title, body, action, RaiseThen(parent, std::move(activate)));
}

void FailedRequest(QWidget* parent, const QString& what, const ApiError& error) {
  const std::optional<error_help::Action> action = error_help::ActionFor(error);
  FailedWithAction(parent, what, QString::fromStdString(error.message), error_help::HintFor(error),
                   action ? action->label : QString(), action ? action->run : std::function<void()>());
}

void Warn(QWidget* parent, const QString& text) {
  if (system_notifier::Send(Urgency::Persistent, "Mira", text)) return;
  ShowCard(parent, Level::Warning, text, /*persistent=*/true);
}

void Notice(QWidget* parent, const QString& text, Level level) {
  if (system_notifier::Send(Urgency::Transient, "Mira", text)) return;
  ShowCard(parent, level, text, /*persistent=*/false);
}

void Info(QWidget* parent, const QString& title, const QString& message) {
  PopupDialog dialog(parent, Level::Info, title);
  dialog.SetMessage(message);
  dialog.AddButton("OK", /*accept_role=*/true, /*default_button=*/true);
  dialog.exec();
}

bool Confirm(QWidget* parent, const QString& title, const QString& question, const QString& accept,
             bool destructive) {
  PopupDialog dialog(parent, destructive ? Level::Warning : Level::Info, title);
  dialog.SetMessage(question);
  // Cancel keeps focus on anything destructive: a stray Return should
  // never pick the dangerous answer.
  QPushButton* go = dialog.AddButton(accept, /*accept_role=*/true, !destructive);
  dialog.AddButton("Cancel", /*accept_role=*/false, destructive);
  Q_UNUSED(go);
  return dialog.exec() == QDialog::Accepted;
}

UnsavedAction ConfirmUnsaved(QWidget* parent, const QString& what) {
  PopupDialog dialog(parent, Level::Warning, "Unsaved changes");
  dialog.SetMessage(what);
  constexpr int kDiscard = static_cast<int>(UnsavedAction::DiscardAndExit);
  constexpr int kSave = static_cast<int>(UnsavedAction::SaveAndExit);
  dialog.AddButton("Save and exit", kSave, /*default_button=*/true);
  dialog.AddButton("Exit without saving", kDiscard, /*default_button=*/false);
  const int result = dialog.exec();
  if (result == kSave) return UnsavedAction::SaveAndExit;
  if (result == kDiscard) return UnsavedAction::DiscardAndExit;
  return UnsavedAction::Cancel;
}

Level LevelFromString(const QString& text) {
  if (text == "success") return Level::Success;
  if (text == "warning") return Level::Warning;
  if (text == "error") return Level::Error;
  return Level::Info;
}

QColor AccentFor(Level level) {
  const theme::Tokens& tokens = theme::Current();
  switch (level) {
    case Level::Success: return tokens.success;
    case Level::Warning: return tokens.warning;
    case Level::Error: return tokens.error;
    case Level::Info: break;
  }
  return tokens.info;
}

}  // namespace mira_gui::notify
