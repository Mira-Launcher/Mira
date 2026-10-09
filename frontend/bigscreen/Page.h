#pragma once

#include <QList>
#include <QWidget>

#include "Paint.h"

namespace mira_gui::bigscreen {

class BigScreenWindow;

// A full screen of big screen mode. Pages paint themselves over the hero
// background and take Nav presses from the window.
class Page : public QWidget {
  Q_OBJECT

public:
  explicit Page(BigScreenWindow* window);

  // Returns false for what the window should handle (Back, tabs, Search).
  virtual bool Navigate(Nav nav) = 0;
  virtual QList<Hint> Hints() const = 0;
  // Called when the page comes to the front.
  virtual void Shown() {}
  // Typing on a real keyboard: `text`, or a backspace when empty. False leaves the key to Navigate.
  virtual bool Typed(const QString& /*text*/) { return false; }

signals:
  void HintsChanged();

protected:
  BigScreenWindow* window_;
};

}  // namespace mira_gui::bigscreen
