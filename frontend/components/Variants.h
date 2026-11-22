#pragma once

namespace mira_gui {

// The color a component takes for what it says: the same set for text, buttons, badges, chips and
// alerts, so "warning" reads alike everywhere.
enum class Tone { Default, Muted, Accent, Info, Success, Warning, Danger };

// The `tone` style property's value base.qss matches, empty for Default.
constexpr const char* ToneName(Tone tone) {
  switch (tone) {
    case Tone::Default:
      return "";
    case Tone::Muted:
      return "muted";
    case Tone::Accent:
      return "accent";
    case Tone::Info:
      return "info";
    case Tone::Success:
      return "success";
    case Tone::Warning:
      return "warning";
    case Tone::Danger:
      return "danger";
  }
  return "";
}

}  // namespace mira_gui
