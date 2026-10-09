#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

#include "NavRepeater.h"

namespace mira_gui::bigscreen {

// Controller input through SDL3, loaded at runtime so builds and packages
// don't depend on it. Without libSDL3, available() is false and nothing is
// ever pressed; big screen then says so and runs on the keyboard. Every
// connected controller works; the last one used is the one shown.
class GamepadInput : public QObject {
  Q_OBJECT

public:
  struct Pad {
    QString name;
    QString kind;  // "xbox", "ps" or "nin", for button labels
    int battery = -1;  // percent, or -1 when the controller doesn't say
    bool charging = false;
    bool wireless = false;
  };
  // What the settings change.
  struct Options {
    bool swap_confirm = false;  // B selects and A goes back, Nintendo style
    int stick_threshold = 18000;  // of 32767, how far a stick moves before it counts
    int first_repeat_ms = 380;
    int repeat_ms = 90;
  };

  explicit GamepadInput(QObject* parent = nullptr);
  ~GamepadInput() override;

  bool available() const { return sdl_ != nullptr; }
  void SetOptions(const Options& options);

  // The controllers connected now, the last used first.
  const std::vector<Pad>& pads() const { return pads_; }
  // The last used controller's labels and name; empty with none.
  QString pad_kind() const { return pads_.empty() ? QString() : pads_.front().kind; }
  QString pad_name() const { return pads_.empty() ? QString() : pads_.front().name; }

  // Vibrates the last used controller; `low` and `high` are the two motors' strengths, 0 to 1.
  void Rumble(double low, double high, int ms);

  // Which buttons are down right now on any controller, for the button test.
  const std::array<bool, size_t(Nav::kCount)>& down() const { return down_; }

signals:
  void Pressed(Nav nav);
  void PadChanged();
  // A controller's battery dropped under 15%.
  void BatteryLow(QString name, int percent);
  // Any button or stick moved, to count as activity.
  void Activity();

private:
  struct Sdl;
  void Poll();
  void Scan();

  std::unique_ptr<Sdl> sdl_;
  QTimer timer_;
  QElapsedTimer clock_;
  NavRepeater repeater_;
  Options options_;
  std::vector<Pad> pads_;
  std::array<bool, size_t(Nav::kCount)> down_{};
  std::int64_t back_since_ = -1;
};

}  // namespace mira_gui::bigscreen
