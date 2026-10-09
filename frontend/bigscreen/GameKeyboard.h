#pragma once

#include <QWidget>

#include "NavRepeater.h"

namespace mira_gui::bigscreen {

class BigScreenWindow;

// An on-screen keyboard over a running game, driven by the controller. It
// never takes focus, so the game keeps it, and types through a virtual
// keyboard (/dev/uinput) the game sees as a real one.
class GameKeyboard : public QWidget {
  Q_OBJECT

public:
  explicit GameKeyboard(BigScreenWindow* window);
  ~GameKeyboard() override;

  // False with a reason when /dev/uinput can't be opened.
  bool Open(QString* error);
  void Navigate(Nav nav);
  // Sends one key press (a Linux KEY_* code, held with `modifier` if set) to whatever has focus.
  // False when uinput isn't available.
  bool Press(int code, QString* error, int modifier = 0);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  struct Key {
    QString label;
    int code;
    double width = 1;
  };
  bool EnsureDevice(QString* error);
  void Type(int code);

  BigScreenWindow* window_;
  std::vector<std::vector<Key>> rows_;
  int row_ = 1;
  int column_ = 0;
  bool shift_ = false;
  int fd_ = -1;
};

}  // namespace mira_gui::bigscreen
