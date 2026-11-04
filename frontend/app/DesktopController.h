#pragma once

#include <QElapsedTimer>
#include <QObject>

#include "../bigscreen/GamepadInput.h"

namespace mira_gui {

struct FrontendPrefs;

// A controller moves around Mira's desktop windows through the focus and keys they already answer
// to: the stick and d-pad move focus, A presses, B is Esc, X opens the menu, Y focuses the search.
// It reads the controller only while a Mira window is in front and big screen is closed, so a game
// in front never drives Mira.
class DesktopController : public QObject {
  Q_OBJECT

 public:
  // Opens the controllers on a worker thread `in_background`, since that takes SDL about 50 ms
  // that would otherwise hold up the window's first paint at launch.
  explicit DesktopController(QObject* parent, bool in_background = true);

  // The controller settings big screen uses: which button selects, stick and repeat.
  void ApplyPrefs(const FrontendPrefs& prefs);
  // Big screen reads the controller itself while it's open.
  void SetBigScreenOpen(bool open);

 signals:
  // Guide tapped: pressed and let go within 1.5 s.
  void OpenBigScreen();

 private:
  void Start(bigscreen::GamepadInput* input);
  void UpdatePaused();
  void Handle(bigscreen::Nav nav);

  bigscreen::GamepadInput* input_ = nullptr;  // once started
  bigscreen::GamepadInput::Options options_;
  bool big_screen_open_ = false;
  QElapsedTimer guide_down_;  // since Guide was pressed, while it's down
};

}  // namespace mira_gui
