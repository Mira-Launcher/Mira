#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mira_gui::bigscreen {

// What a controller or the keyboard asks big screen to do.
enum class Nav { Up, Down, Left, Right, Accept, Back, Action, Search, PrevTab, NextTab, Guide, Sort,
                 // Triggers: the previous or next letter in a list sorted by name.
                 PrevLetter, NextLetter,
                 // B held down: straight to Home.
                 Home, kCount };

// Turns sampled button states into presses: one on the way down, and for
// directions, repeats while held (a pause first, then steadily).
class NavRepeater {
public:
  static constexpr std::int64_t kFirstRepeatMs = 380;
  static constexpr std::int64_t kRepeatMs = 90;

  // How soon a held direction repeats, and how often after that.
  void SetTiming(std::int64_t first_ms, std::int64_t repeat_ms) {
    first_ms_ = first_ms;
    repeat_ms_ = repeat_ms;
  }

  std::vector<Nav> Feed(const std::array<bool, size_t(Nav::kCount)>& down, std::int64_t now_ms) {
    std::vector<Nav> out;
    for (size_t i = 0; i < down.size(); ++i) {
      const Nav nav = Nav(i);
      if (!down[i]) {
        held_[i] = false;
        continue;
      }
      if (!held_[i]) {
        held_[i] = true;
        next_[i] = now_ms + first_ms_;
        out.push_back(nav);
      } else if (IsDirection(nav) && now_ms >= next_[i]) {
        next_[i] = now_ms + repeat_ms_;
        out.push_back(nav);
      }
    }
    return out;
  }

  static bool IsDirection(Nav nav) { return nav <= Nav::Right; }

private:
  std::int64_t first_ms_ = kFirstRepeatMs;
  std::int64_t repeat_ms_ = kRepeatMs;
  std::array<bool, size_t(Nav::kCount)> held_{};
  std::array<std::int64_t, size_t(Nav::kCount)> next_{};
};

}  // namespace mira_gui::bigscreen
