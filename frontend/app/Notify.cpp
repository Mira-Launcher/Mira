#include "Notify.h"

#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include "../components/Button.h"
#include "../components/PopupDialog.h"
#include "../components/Variants.h"
#include "../theme/Theme.h"
#include "SystemNotifier.h"

namespace mira_gui::notify {
namespace {

using system_notifier::Urgency;

// A blank line apart in a notification or popup; a compact Activity row asks for one line break.
QString JoinDetail(const QString& detail, const QString& hint, const char* gap = "\n\n") {
  if (detail.isEmpty()) return hint;
  if (hint.isEmpty()) return detail;
  return detail + gap + hint;
}

Tone ToneOf(Level level) {
  switch (level) {
    case Level::Success:
      return Tone::Success;
    case Level::Warning:
      return Tone::Warning;
    case Level::Error:
      return Tone::Danger;
    case Level::Info:
      break;
  }
  return Tone::Info;
}

Fallback& CurrentFallback() {
  static Fallback fallback;
  return fallback;
}

// For when no notification service took it: the owner's fallback (Activity), else a popup.
void Undelivered(QWidget* parent, Level level, const QString& what, const QString& detail,
                 const QString& hint, const QString& action, std::function<void()> activate,
                 const ApiError& cause = {}) {
  if (const Fallback& fallback = CurrentFallback()) {
    fallback(level, what, JoinDetail(detail, hint, "\n"), cause);
    return;
  }
  PopupDialog dialog(parent, ToneOf(level), "Mira");
  dialog.SetMessage(what);
  if (const QString body = JoinDetail(detail, hint); !body.isEmpty()) dialog.SetDetail(body);
  if (!action.isEmpty() && activate) dialog.SetAction(action, std::move(activate));
  dialog.AddButton("OK", /*accept_role=*/true, /*default_button=*/true);
  dialog.exec();
}

}  // namespace

void SetFallback(Fallback fallback) {
  CurrentFallback() = std::move(fallback);
}

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

void FailedBecause(QWidget* parent, const QString& what, const QString& detail, const QString& hint,
                   const QString& action, std::function<void()> activate, const ApiError& cause) {
  const std::function<void()> on_action =
      activate ? RaiseThen(parent, activate) : std::function<void()>();
  if (system_notifier::Send(Urgency::Persistent, what, JoinDetail(detail, hint), action, on_action))
    return;
  Undelivered(parent, Level::Error, what, detail, hint, action, std::move(activate), cause);
}

}  // namespace

void FailedWithAction(QWidget* parent, const QString& what, const QString& detail,
                      const QString& hint, const QString& action, std::function<void()> activate) {
  FailedBecause(parent, what, detail, hint, action, std::move(activate), {});
}

bool AskOutOfSight(QWidget* parent, const QString& title, const QString& body,
                   const QString& action, std::function<void()> activate) {
  return system_notifier::Send(Urgency::Persistent, title, body, action,
                               RaiseThen(parent, std::move(activate)));
}

void FailedRequest(QWidget* parent, const QString& what, const ApiError& error) {
  const std::optional<error_help::Action> action = error_help::ActionFor(error);
  FailedBecause(parent, what, QString::fromStdString(error.message), error_help::HintFor(error),
                action ? action->label : QString(), action ? action->run : std::function<void()>(),
                error);
}

void Warn(QWidget* parent, const QString& text) {
  if (system_notifier::Send(Urgency::Persistent, "Mira", text)) return;
  Undelivered(parent, Level::Warning, text, QString(), QString(), QString(), {});
}

void Warn(QWidget* parent, const QString& text, const ApiError& error) {
  const std::optional<error_help::Action> action = error_help::ActionFor(error);
  const QString hint = error_help::HintFor(error);
  if (system_notifier::Send(Urgency::Persistent, text, hint, action ? action->label : QString(),
                            action ? RaiseThen(parent, action->run) : std::function<void()>()))
    return;
  Undelivered(parent, Level::Warning, text, QString(), hint, action ? action->label : QString(),
              action ? action->run : std::function<void()>(), error);
}

void Notice(QWidget* parent, const QString& text, Level level) {
  if (system_notifier::Send(Urgency::Transient, "Mira", text)) return;
  if (level == Level::Warning || level == Level::Error)
    Undelivered(parent, level, text, QString(), QString(), QString(), {});
}

void Info(QWidget* parent, const QString& title, const QString& message) {
  PopupDialog dialog(parent, Tone::Info, title);
  dialog.SetMessage(message);
  dialog.AddButton("OK", /*accept_role=*/true, /*default_button=*/true);
  dialog.exec();
}

bool Confirm(QWidget* parent, const QString& title, const QString& question, const QString& accept,
             bool destructive) {
  PopupDialog dialog(parent, destructive ? Tone::Warning : Tone::Info, title);
  dialog.SetMessage(question);
  // Cancel keeps focus on anything destructive: a stray Return should
  // never pick the dangerous answer.
  Button* go = dialog.AddButton(accept, /*accept_role=*/true, !destructive);
  dialog.AddButton("Cancel", /*accept_role=*/false, destructive);
  if (destructive) go->SetKind(ButtonKind::Danger);
  return dialog.exec() == QDialog::Accepted;
}

bool LeaveUnsaved(QWidget* parent, const QString& what, const std::function<void()>& save) {
  PopupDialog dialog(parent, Tone::Warning, "Unsaved changes");
  dialog.SetMessage(what);
  constexpr int kSave = 1;
  constexpr int kDiscard = 2;
  dialog.AddButton("Save and exit", kSave, /*default_button=*/true);
  dialog.AddButton("Exit without saving", kDiscard, /*default_button=*/false);
  const int result = dialog.exec();
  if (result == kSave) save();
  return result == kDiscard;
}

Level LevelFromString(const QString& text) {
  if (text == "success") return Level::Success;
  if (text == "warning") return Level::Warning;
  if (text == "error") return Level::Error;
  return Level::Info;
}

QColor AccentFor(Level level) {
  return ToneColor(ToneOf(level));
}

}  // namespace mira_gui::notify
