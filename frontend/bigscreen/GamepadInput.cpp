#include "GamepadInput.h"

#include <QLibrary>

#include <cstdint>

namespace mira_gui::bigscreen {
namespace {

// The few SDL3 declarations used, from SDL_gamepad.h and SDL_init.h. SDL3
// keeps its ABI stable across 3.x, so these hold for any libSDL3.so.0.
struct SDL_Gamepad;
using SDL_JoystickID = std::uint32_t;
constexpr std::uint32_t kInitGamepad = 0x00002000u;
enum Button { kSouth, kEast, kWest, kNorth, kBack, kGuide, kStart, kLeftStick, kRightStick,
              kLeftShoulder, kRightShoulder, kDpadUp, kDpadDown, kDpadLeft, kDpadRight };
enum Axis { kLeftX, kLeftY };
enum Type { kTypeUnknown, kTypeStandard, kTypeXbox360, kTypeXboxOne, kTypePs3, kTypePs4, kTypePs5,
            kTypeSwitchPro, kTypeJoyconLeft, kTypeJoyconRight, kTypeJoyconPair, kTypeGamecube };
constexpr std::int16_t kStickThreshold = 18000;  // of 32767

}  // namespace

struct GamepadInput::Sdl {
  QLibrary lib;
  bool (*SetHint)(const char*, const char*) = nullptr;
  bool (*Init)(std::uint32_t) = nullptr;
  void (*QuitSubSystem)(std::uint32_t) = nullptr;
  void (*SetGamepadEventsEnabled)(bool) = nullptr;
  void (*UpdateGamepads)() = nullptr;
  SDL_JoystickID* (*GetGamepads)(int*) = nullptr;
  void (*Free)(void*) = nullptr;
  SDL_Gamepad* (*OpenGamepad)(SDL_JoystickID) = nullptr;
  void (*CloseGamepad)(SDL_Gamepad*) = nullptr;
  bool (*GamepadConnected)(SDL_Gamepad*) = nullptr;
  bool (*GetGamepadButton)(SDL_Gamepad*, int) = nullptr;
  std::int16_t (*GetGamepadAxis)(SDL_Gamepad*, int) = nullptr;
  int (*GetGamepadType)(SDL_Gamepad*) = nullptr;
  const char* (*GetGamepadName)(SDL_Gamepad*) = nullptr;

  SDL_Gamepad* pad = nullptr;
  int scan_countdown = 0;

  bool Load() {
    lib.setFileNameAndVersion("SDL3", 0);
    if (!lib.load()) return false;
    bool ok = true;
    const auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib.resolve(name));
      ok = ok && fn != nullptr;
    };
    get(SetHint, "SDL_SetHint");
    get(Init, "SDL_Init");
    get(QuitSubSystem, "SDL_QuitSubSystem");
    get(SetGamepadEventsEnabled, "SDL_SetGamepadEventsEnabled");
    get(UpdateGamepads, "SDL_UpdateGamepads");
    get(GetGamepads, "SDL_GetGamepads");
    get(Free, "SDL_free");
    get(OpenGamepad, "SDL_OpenGamepad");
    get(CloseGamepad, "SDL_CloseGamepad");
    get(GamepadConnected, "SDL_GamepadConnected");
    get(GetGamepadButton, "SDL_GetGamepadButton");
    get(GetGamepadAxis, "SDL_GetGamepadAxis");
    get(GetGamepadType, "SDL_GetGamepadType");
    get(GetGamepadName, "SDL_GetGamepadName");
    if (!ok) return false;
    // Guide must still reach Mira while a game has focus.
    SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    // SDL would otherwise swallow SIGINT and SIGTERM, so mira-gui couldn't be stopped.
    SetHint("SDL_NO_SIGNAL_HANDLERS", "1");
    if (!Init(kInitGamepad)) return false;
    // Polled below, so nothing should queue up as events.
    SetGamepadEventsEnabled(false);
    return true;
  }

  ~Sdl() {
    if (pad != nullptr) CloseGamepad(pad);
    if (QuitSubSystem != nullptr && lib.isLoaded()) QuitSubSystem(kInitGamepad);
  }
};

GamepadInput::GamepadInput(QObject* parent) : QObject(parent) {
  auto sdl = std::make_unique<Sdl>();
  if (!sdl->Load()) return;
  sdl_ = std::move(sdl);
  clock_.start();
  timer_.setInterval(8);
  connect(&timer_, &QTimer::timeout, this, &GamepadInput::Poll);
  timer_.start();
}

GamepadInput::~GamepadInput() = default;

void GamepadInput::Poll() {
  Sdl& sdl = *sdl_;
  sdl.UpdateGamepads();
  if (sdl.pad != nullptr && !sdl.GamepadConnected(sdl.pad)) {
    sdl.CloseGamepad(sdl.pad);
    sdl.pad = nullptr;
    pad_kind_.clear();
    pad_name_.clear();
    emit PadChanged();
  }
  if (sdl.pad == nullptr) {
    // Looking for a new controller twice a second is plenty.
    if (sdl.scan_countdown-- > 0) return;
    sdl.scan_countdown = 60;
    int count = 0;
    SDL_JoystickID* ids = sdl.GetGamepads(&count);
    if (ids != nullptr && count > 0) sdl.pad = sdl.OpenGamepad(ids[0]);
    if (ids != nullptr) sdl.Free(ids);
    if (sdl.pad == nullptr) return;
    const int type = sdl.GetGamepadType(sdl.pad);
    pad_kind_ = type >= kTypePs3 && type <= kTypePs5          ? "ps"
                : type >= kTypeSwitchPro && type <= kTypeGamecube ? "nin"
                                                                  : "xbox";
    pad_name_ = QString::fromUtf8(sdl.GetGamepadName(sdl.pad));
    emit PadChanged();
  }

  const auto button = [&](int b) { return sdl.GetGamepadButton(sdl.pad, b); };
  const std::int16_t x = sdl.GetGamepadAxis(sdl.pad, kLeftX);
  const std::int16_t y = sdl.GetGamepadAxis(sdl.pad, kLeftY);
  std::array<bool, size_t(Nav::kCount)> down{};
  down[size_t(Nav::Up)] = button(kDpadUp) || y < -kStickThreshold;
  down[size_t(Nav::Down)] = button(kDpadDown) || y > kStickThreshold;
  down[size_t(Nav::Left)] = button(kDpadLeft) || x < -kStickThreshold;
  down[size_t(Nav::Right)] = button(kDpadRight) || x > kStickThreshold;
  down[size_t(Nav::Accept)] = button(kSouth);
  down[size_t(Nav::Back)] = button(kEast);
  down[size_t(Nav::Action)] = button(kWest);
  down[size_t(Nav::Search)] = button(kNorth);
  down[size_t(Nav::PrevTab)] = button(kLeftShoulder);
  down[size_t(Nav::NextTab)] = button(kRightShoulder);
  down[size_t(Nav::Guide)] = button(kGuide);
  for (const Nav nav : repeater_.Feed(down, clock_.elapsed())) emit Pressed(nav);
}

}  // namespace mira_gui::bigscreen
