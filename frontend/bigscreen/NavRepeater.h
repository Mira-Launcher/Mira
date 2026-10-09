#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mira_gui::bigscreen {

// What a controller or the keyboard asks big screen to do.
enum class Nav { Up, Down, Left, Right, Accept, Back, Action, Search, PrevTab, NextTab, Guide, Sort, kCount };

// Turns sampled button states into presses: one on the way down, and for
// directions, repeats while held (a pause first, then steadily).
class NavRepeater {
public:
  static constexpr std::int64_t kFirstRepeatMs = 380;
  static constexpr std::int64_t kRepeatMs = 90;

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
        next_[i] = now_ms + kFirstRepeatMs;
        out.push_back(nav);
      } else if (IsDirection(nav) && now_ms >= next_[i]) {
        next_[i] = now_ms + kRepeatMs;
        out.push_back(nav);
      }
    }
    return out;
  }

  static bool IsDirection(Nav nav) { return nav <= Nav::Right; }

private:
  std::array<bool, size_t(Nav::kCount)> held_{};
  std::array<std::int64_t, size_t(Nav::kCount)> next_{};
};

}  // namespace mira_gui::bigscreen
