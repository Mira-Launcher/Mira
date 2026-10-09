#include "GamepadInput.h"

#include <QLibrary>

#include <algorithm>
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
enum Axis { kLeftX, kLeftY, kRightX, kRightY, kLeftTrigger, kRightTrigger };
enum Type { kTypeUnknown, kTypeStandard, kTypeXbox360, kTypeXboxOne, kTypePs3, kTypePs4, kTypePs5,
            kTypeSwitchPro, kTypeJoyconLeft, kTypeJoyconRight, kTypeJoyconPair, kTypeGamecube };
enum PowerState { kPowerOnBattery = 1, kPowerNoBattery, kPowerCharging, kPowerCharged };
constexpr int kConnectionWireless = 2;
constexpr std::int16_t kTriggerThreshold = 16000;
constexpr std::int64_t kHoldBackMs = 700;

}  // namespace

struct GamepadInput::Sdl {
  QLibrary lib;
  bool (*SetHint)(const char*, const char*) = nullptr;
  bool (*Init)(std::uint32_t) = nullptr;
  void (*QuitSubSystem)(std::uint32_t) = nullptr;
  void (*SetGamepadEventsEnabled)(bool) = nullptr;
  void (*UpdateGamepads)() = nullptr;
  void (*PumpEvents)() = nullptr;
  void (*FlushEvents)(std::uint32_t, std::uint32_t) = nullptr;
  SDL_JoystickID* (*GetGamepads)(int*) = nullptr;
  void (*Free)(void*) = nullptr;
  SDL_Gamepad* (*OpenGamepad)(SDL_JoystickID) = nullptr;
  void (*CloseGamepad)(SDL_Gamepad*) = nullptr;
  bool (*GamepadConnected)(SDL_Gamepad*) = nullptr;
  bool (*GetGamepadButton)(SDL_Gamepad*, int) = nullptr;
  std::int16_t (*GetGamepadAxis)(SDL_Gamepad*, int) = nullptr;
  int (*GetGamepadType)(SDL_Gamepad*) = nullptr;
  const char* (*GetGamepadName)(SDL_Gamepad*) = nullptr;
  bool (*RumbleGamepad)(SDL_Gamepad*, std::uint16_t, std::uint16_t, std::uint32_t) = nullptr;
  int (*GetGamepadPowerInfo)(SDL_Gamepad*, int*) = nullptr;
  int (*GetGamepadConnectionState)(SDL_Gamepad*) = nullptr;
  SDL_JoystickID (*GetGamepadID)(SDL_Gamepad*) = nullptr;

  struct Open {
    SDL_Gamepad* pad;
    bool warned_low = false;
  };
  // In the same order as GamepadInput::pads_.
  std::vector<Open> open;
  int scan_countdown = 0;
  int power_countdown = 0;

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
    get(PumpEvents, "SDL_PumpEvents");
    get(FlushEvents, "SDL_FlushEvents");
    get(GetGamepads, "SDL_GetGamepads");
    get(Free, "SDL_free");
    get(OpenGamepad, "SDL_OpenGamepad");
    get(CloseGamepad, "SDL_CloseGamepad");
    get(GamepadConnected, "SDL_GamepadConnected");
    get(GetGamepadButton, "SDL_GetGamepadButton");
    get(GetGamepadAxis, "SDL_GetGamepadAxis");
    get(GetGamepadType, "SDL_GetGamepadType");
    get(GetGamepadName, "SDL_GetGamepadName");
    get(RumbleGamepad, "SDL_RumbleGamepad");
    get(GetGamepadPowerInfo, "SDL_GetGamepadPowerInfo");
    get(GetGamepadConnectionState, "SDL_GetGamepadConnectionState");
    get(GetGamepadID, "SDL_GetGamepadID");
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
    for (const Open& pad : open) CloseGamepad(pad.pad);
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

void GamepadInput::SetOptions(const Options& options) {
  options_ = options;
  repeater_.SetTiming(options.first_repeat_ms, options.repeat_ms);
}

void GamepadInput::Scan() {
  Sdl& sdl = *sdl_;
  bool changed = false;
  // Drop the ones unplugged.
  for (size_t i = sdl.open.size(); i-- > 0;) {
    if (sdl.GamepadConnected(sdl.open[i].pad)) continue;
    sdl.CloseGamepad(sdl.open[i].pad);
    sdl.open.erase(sdl.open.begin() + std::ptrdiff_t(i));
    pads_.erase(pads_.begin() + std::ptrdiff_t(i));
    changed = true;
  }
  // Looking for new controllers twice a second is plenty.
  if (sdl.scan_countdown-- <= 0) {
    sdl.scan_countdown = 60;
    // Only pumping events finds controllers plugged in since; the events themselves aren't used.
    sdl.PumpEvents();
    sdl.FlushEvents(0, 0xffffffff);
    int count = 0;
    SDL_JoystickID* ids = sdl.GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
      const bool known = std::ranges::any_of(sdl.open, [&](const Sdl::Open& pad) { return sdl.GetGamepadID(pad.pad) == ids[i]; });
      if (known) continue;
      SDL_Gamepad* pad = sdl.OpenGamepad(ids[i]);
      if (pad == nullptr) continue;
      const int type = sdl.GetGamepadType(pad);
      Pad info;
      info.kind = type >= kTypePs3 && type <= kTypePs5                ? "ps"
                  : type >= kTypeSwitchPro && type <= kTypeGamecube ? "nin"
                                                                    : "xbox";
      info.name = QString::fromUtf8(sdl.GetGamepadName(pad));
      sdl.open.push_back({pad});
      pads_.push_back(info);
      sdl.power_countdown = 0;
      changed = true;
    }
    if (ids != nullptr) sdl.Free(ids);
  }
  // The battery changes slowly; every two seconds is enough.
  if (sdl.power_countdown-- <= 0) {
    sdl.power_countdown = 250;
    for (size_t i = 0; i < sdl.open.size(); ++i) {
      int percent = -1;
      const int state = sdl.GetGamepadPowerInfo(sdl.open[i].pad, &percent);
      Pad& pad = pads_[i];
      const int battery = state == kPowerOnBattery || state == kPowerCharging || state == kPowerCharged ? percent : -1;
      const bool charging = state == kPowerCharging || state == kPowerCharged;
      const bool wireless = sdl.GetGamepadConnectionState(sdl.open[i].pad) == kConnectionWireless;
      if (battery != pad.battery || charging != pad.charging || wireless != pad.wireless) changed = true;
      pad.battery = battery;
      pad.charging = charging;
      pad.wireless = wireless;
      const bool low = battery >= 0 && battery < 15 && !charging;
      if (low && !sdl.open[i].warned_low) emit BatteryLow(pad.name, battery);
      // Warn again only after it's been charged.
      if (!low && battery >= 25) sdl.open[i].warned_low = false;
      if (low) sdl.open[i].warned_low = true;
    }
  }
  if (changed) emit PadChanged();
}

void GamepadInput::Poll() {
  Sdl& sdl = *sdl_;
  sdl.UpdateGamepads();
  Scan();
  if (sdl.open.empty()) return;

  std::array<bool, size_t(Nav::kCount)> down{};
  const std::int16_t threshold = std::int16_t(options_.stick_threshold);
  int used = -1;
  for (size_t p = 0; p < sdl.open.size(); ++p) {
    SDL_Gamepad* pad = sdl.open[p].pad;
    const auto button = [&](int b) { return sdl.GetGamepadButton(pad, b); };
    const auto axis = [&](int a) { return sdl.GetGamepadAxis(pad, a); };
    std::array<bool, size_t(Nav::kCount)> mine{};
    const std::int16_t x = axis(kLeftX), y = axis(kLeftY);
    mine[size_t(Nav::Up)] = button(kDpadUp) || y < -threshold;
    mine[size_t(Nav::Down)] = button(kDpadDown) || y > threshold;
    mine[size_t(Nav::Left)] = button(kDpadLeft) || x < -threshold;
    mine[size_t(Nav::Right)] = button(kDpadRight) || x > threshold;
    mine[size_t(options_.swap_confirm ? Nav::Back : Nav::Accept)] = button(kSouth);
    mine[size_t(options_.swap_confirm ? Nav::Accept : Nav::Back)] = button(kEast);
    mine[size_t(Nav::Action)] = button(kWest);
    mine[size_t(Nav::Search)] = button(kNorth);
    mine[size_t(Nav::PrevTab)] = button(kLeftShoulder);
    mine[size_t(Nav::NextTab)] = button(kRightShoulder);
    mine[size_t(Nav::Guide)] = button(kGuide);
    mine[size_t(Nav::Sort)] = button(kBack);
    mine[size_t(Nav::PrevLetter)] = axis(kLeftTrigger) > kTriggerThreshold;
    mine[size_t(Nav::NextLetter)] = axis(kRightTrigger) > kTriggerThreshold;
    for (size_t i = 0; i < down.size(); ++i) {
      if (!mine[i]) continue;
      down[i] = true;
      // A button newly down marks the controller in use.
      if (!down_[i] && used < 0) used = int(p);
    }
  }
  const std::int64_t now = clock_.elapsed();
  // Held B goes Home, once per hold.
  if (down[size_t(Nav::Back)]) {
    if (back_since_ < 0) back_since_ = now;
    if (back_since_ > 0 && now - back_since_ >= kHoldBackMs) {
      back_since_ = 0;
      emit Pressed(Nav::Home);
    }
  } else {
    back_since_ = -1;
  }
  if (used > 0) {
    // The last used controller goes first, so its labels and battery show.
    std::rotate(sdl.open.begin(), sdl.open.begin() + used, sdl.open.begin() + used + 1);
    std::rotate(pads_.begin(), pads_.begin() + used, pads_.begin() + used + 1);
    emit PadChanged();
  }
  if (down != down_) emit Activity();
  down_ = down;
  for (const Nav nav : repeater_.Feed(down, now)) emit Pressed(nav);
}

void GamepadInput::Rumble(double low, double high, int ms) {
  if (!sdl_ || sdl_->open.empty()) return;
  const auto strength = [](double v) { return std::uint16_t(std::clamp(v, 0.0, 1.0) * 65535); };
  sdl_->RumbleGamepad(sdl_->open.front().pad, strength(low), strength(high), std::uint32_t(ms));
}

}  // namespace mira_gui::bigscreen
