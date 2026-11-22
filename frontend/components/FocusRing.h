#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

namespace mira_gui {

// One accent ring on the edge of whatever has focus, in every window. A list's current item gets it
// only when the list's own selection doesn't already mark it. It shows once focus moves by keyboard
// or controller and hides at the next mouse press, so pointing and clicking looks as it always did.
class FocusRing : public QObject {
  Q_OBJECT

 public:
  // Watches the whole application; call once, after the QApplication exists.
  static void Install();

  // A window that marks focus its own way (big screen) never gets the ring, nor arrow keys that
  // move focus by position.
  static void LeaveOut(QWidget* window);
  static bool LeftOut(const QWidget* window);

  // The ring's rectangle around `widget`, in its window's coordinates, before it's clipped.
  static QRect Around(QWidget* widget);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  explicit FocusRing(QObject* parent);
  void Follow(QWidget* focus);
  void Place();
  void PlaceSoon();
  void SetShown(bool shown);

  class Ring;
  QPointer<QWidget> focus_;
  QPointer<Ring> ring_;
  QList<QPointer<QObject>> watched_;  // the focused control and what holds it
  QPointer<QObject> painted_;         // a focused list's viewport
  bool shown_ = false;
  bool place_queued_ = false;
};

}  // namespace mira_gui
