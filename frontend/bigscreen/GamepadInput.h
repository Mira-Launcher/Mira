#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "NavRepeater.h"

namespace mira_gui::bigscreen {

// Controller input through SDL3, loaded at runtime so builds and packages
// don't depend on it. Without libSDL3, available() is false and nothing is
// ever pressed; big screen then says so and runs on the keyboard.
class GamepadInput : public QObject {
  Q_OBJECT

public:
  explicit GamepadInput(QObject* parent = nullptr);
  ~GamepadInput() override;

  bool available() const { return sdl_ != nullptr; }
  // "xbox", "ps" or "nin" for the open controller's button labels; empty with none.
  QString pad_kind() const { return pad_kind_; }
  QString pad_name() const { return pad_name_; }
  // Battery percent, or -1 when the controller doesn't say (wired ones mostly).
  int battery() const { return battery_; }
  bool charging() const { return charging_; }
  bool wireless() const { return wireless_; }

  // Vibrates the open controller; `low` and `high` are the two motors' strengths, 0 to 1.
  void Rumble(double low, double high, int ms);

signals:
  void Pressed(Nav nav);
  void PadChanged();

private:
  struct Sdl;
  void Poll();

  std::unique_ptr<Sdl> sdl_;
  QTimer timer_;
  QElapsedTimer clock_;
  NavRepeater repeater_;
  QString pad_kind_;
  QString pad_name_;
  int battery_ = -1;
  bool charging_ = false;
  bool wireless_ = false;
};

}  // namespace mira_gui::bigscreen
