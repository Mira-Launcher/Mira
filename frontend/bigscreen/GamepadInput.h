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
};

}  // namespace mira_gui::bigscreen
