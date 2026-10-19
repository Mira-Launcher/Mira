#pragma once

#include <QDialog>
#include <QString>

#include "../app/Notify.h"

class QLabel;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;
class QPushButton;

namespace mira_gui {

class SettingsCard;

// Themed QMessageBox replacement: the same card as every other in-window card, over the window
// it was asked from with the rest dimmed, so a question never looks like another program's
// window. Asked with no window shown (Mira in the tray), it's a frameless window of its own.
//
// mira_gui::notify::Failed/Info/Confirm/… are built on this; construct it
// directly only for a shape none of them cover.
class PopupDialog : public QDialog {
  Q_OBJECT

 public:
  PopupDialog(QWidget* parent, notify::Level level, const QString& title);

  void SetMessage(const QString& text);

  // mirad's own message or other detail, muted, under the message. Omitted
  // unless called.
  void SetDetail(const QString& text);

  // A widget of the caller's under the message and detail.
  void AddWidget(QWidget* widget);
  // Wider than a message needs, for a form.
  void SetWidth(int width);

  // Clickable link under the detail, for a popup that names where to go fix
  // the problem. Opens `url` in the desktop's browser.
  void SetAction(const QString& label, const QString& url);
  // Same, but runs `activated` instead of opening a URL. Call at most one
  // SetAction overload.
  void SetAction(const QString& label, std::function<void()> activated);

  // A button that closes the dialog; exec() then returns QDialog::Accepted or ::Rejected per
  // `accept`. Buttons sit on the right, those that reject first, then the rest in the order
  // added with the Enter-activated `default_button` last.
  QPushButton* AddButton(const QString& text, bool accept, bool default_button = false);
  // Same, but exec() returns `result` verbatim, for a caller with more than
  // two outcomes (see notify::LeaveUnsaved).
  QPushButton* AddButton(const QString& text, int result, bool default_button = false);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

 private:
  bool overlay_ = false;  // over its window, rather than a window of its own
  SettingsCard* card_ = nullptr;
  QVBoxLayout* body_ = nullptr;
  QHBoxLayout* buttons_ = nullptr;
  QLabel* message_ = nullptr;
  QLabel* detail_ = nullptr;
  QLabel* action_ = nullptr;
  QPushButton* default_ = nullptr;  // has the focus
  QPushButton* last_ = nullptr;     // the default, unless it rejects: it ends the row
};

}  // namespace mira_gui
