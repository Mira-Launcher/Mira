#include "Sounds.h"

#include <QLibrary>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace mira_gui::bigscreen {
namespace {

// From SDL_audio.h and SDL_init.h.
struct SDL_AudioStream;
struct AudioSpec {
  int format;
  int channels;
  int freq;
};
constexpr std::uint32_t kInitAudio = 0x00000010u;
constexpr std::uint32_t kDefaultPlayback = 0xFFFFFFFFu;
constexpr int kF32 = 0x8120;
constexpr int kRate = 48000;

// A soft tone per note: a quick fade in and out so it never clicks.
std::vector<float> Tones(std::initializer_list<std::pair<double, double>> notes, float volume) {
  std::vector<float> out;
  for (const auto& [hz, seconds] : notes) {
    const int count = int(seconds * kRate);
    for (int i = 0; i < count; ++i) {
      const double t = double(i) / kRate;
      const double envelope = std::min({1.0, t / 0.004, (seconds - t) / 0.025});
      out.push_back(float(volume * envelope * std::sin(2 * std::numbers::pi * hz * t)));
    }
  }
  return out;
}

}  // namespace

struct Sounds::Sdl {
  QLibrary lib;
  bool (*Init)(std::uint32_t) = nullptr;
  void (*QuitSubSystem)(std::uint32_t) = nullptr;
  SDL_AudioStream* (*OpenAudioDeviceStream)(std::uint32_t, const AudioSpec*, void*, void*) = nullptr;
  bool (*ResumeAudioStreamDevice)(SDL_AudioStream*) = nullptr;
  bool (*PauseAudioStreamDevice)(SDL_AudioStream*) = nullptr;
  bool (*PutAudioStreamData)(SDL_AudioStream*, const void*, int) = nullptr;
  bool (*ClearAudioStream)(SDL_AudioStream*) = nullptr;
  void (*DestroyAudioStream)(SDL_AudioStream*) = nullptr;
  SDL_AudioStream* stream = nullptr;
  std::vector<float> cues[5];

  bool Load() {
    lib.setFileNameAndVersion("SDL3", 0);
    if (!lib.load()) return false;
    bool ok = true;
    const auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib.resolve(name));
      ok = ok && fn != nullptr;
    };
    get(Init, "SDL_Init");
    get(QuitSubSystem, "SDL_QuitSubSystem");
    get(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream");
    get(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice");
    get(PauseAudioStreamDevice, "SDL_PauseAudioStreamDevice");
    get(PutAudioStreamData, "SDL_PutAudioStreamData");
    get(ClearAudioStream, "SDL_ClearAudioStream");
    get(DestroyAudioStream, "SDL_DestroyAudioStream");
    if (!ok || !Init(kInitAudio)) return false;
    const AudioSpec spec{kF32, 1, kRate};
    stream = OpenAudioDeviceStream(kDefaultPlayback, &spec, nullptr, nullptr);
    if (stream == nullptr) {
      QuitSubSystem(kInitAudio);
      return false;
    }
    cues[int(Cue::Move)] = Tones({{1320, 0.035}}, 0.05f);
    cues[int(Cue::Accept)] = Tones({{880, 0.05}, {1320, 0.07}}, 0.08f);
    cues[int(Cue::Back)] = Tones({{1100, 0.05}, {740, 0.07}}, 0.07f);
    cues[int(Cue::Bump)] = Tones({{220, 0.06}}, 0.09f);
    cues[int(Cue::Tab)] = Tones({{990, 0.03}, {1480, 0.045}}, 0.06f);
    return true;
  }

  ~Sdl() {
    if (stream != nullptr) {
      DestroyAudioStream(stream);
      QuitSubSystem(kInitAudio);
    }
  }
};

Sounds::Sounds(QObject* parent) : QObject(parent) {
  auto sdl = std::make_unique<Sdl>();
  if (sdl->Load()) sdl_ = std::move(sdl);
  idle_.setSingleShot(true);
  idle_.setInterval(3000);
  connect(&idle_, &QTimer::timeout, this, [this] { sdl_->PauseAudioStreamDevice(sdl_->stream); });
}

Sounds::~Sounds() = default;

void Sounds::Play(Cue cue) {
  if (!sdl_) return;
  const std::vector<float>& samples = sdl_->cues[int(cue)];
  // Held directions shouldn't queue up a backlog of ticks.
  sdl_->ClearAudioStream(sdl_->stream);
  sdl_->PutAudioStreamData(sdl_->stream, samples.data(), int(samples.size() * sizeof(float)));
  sdl_->ResumeAudioStreamDevice(sdl_->stream);
  idle_.start();
}

}  // namespace mira_gui::bigscreen
