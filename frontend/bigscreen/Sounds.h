#pragma once

#include <QTimer>

#include <memory>

namespace mira_gui::bigscreen {

// Short navigation sounds, generated rather than shipped, played through
// SDL3's audio (loaded at runtime like the controller). Silent without SDL3.
class Sounds : public QObject {
  Q_OBJECT

public:
  enum class Cue { Move, Accept, Back, Bump, Tab };

  explicit Sounds(QObject* parent = nullptr);
  ~Sounds() override;

  void Play(Cue cue);

private:
  struct Sdl;
  std::unique_ptr<Sdl> sdl_;
  // Lets go of the audio device once things are quiet.
  QTimer idle_;
};

}  // namespace mira_gui::bigscreen
