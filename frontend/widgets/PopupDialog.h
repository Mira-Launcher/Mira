#pragma once

#include <QDialog>
#include <QString>

#include "Notify.h"

class QLabel;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;

namespace mira_gui {

// Themed QMessageBox replacement. A native QDialog gets WM decorations of
// its own on top of a window that has none (LibraryWindow is frameless),
// two window styles stacked, the "popup inside a popup" look. This draws
// its own frameless, rounded, themed card instead.
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

  // Clickable link under the detail, for a popup that names where to go fix
  // the problem. Opens `url` in the desktop's browser.
  void SetAction(const QString& label, const QString& url);
  // Same, but runs `activated` instead of opening a URL. Call at most one
  // SetAction overload.
  void SetAction(const QString& label, std::function<void()> activated);

  // First button added is Tab-order first, and the Enter-activated default
  // unless a later one passes `default_button`. Closes the dialog on click;
  // exec() then returns QDialog::Accepted or ::Rejected per `accept`.
  QPushButton* AddButton(const QString& text, bool accept, bool default_button = false);
  // Same, but exec() returns `result` verbatim, for a caller with more than
  // two outcomes (see notify::ConfirmUnsaved).
  QPushButton* AddButton(const QString& text, int result, bool default_button = false);

protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  QVBoxLayout* body_ = nullptr;
  QHBoxLayout* buttons_ = nullptr;
  QLabel* message_ = nullptr;
  QLabel* detail_ = nullptr;
  QLabel* action_ = nullptr;
  QToolButton* close_button_ = nullptr;
  notify::Level level_;
};

}  // namespace mira_gui
