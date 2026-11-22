#pragma once

#include <QColor>

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

// The color a tone paints text, outlines and dots in, from the current theme; Default is the text
// color.
QColor ToneColor(Tone tone);

// A color of its own (a ProtonDB tier, a Steam review group) as text or an outline on the theme's
// surfaces: lighter on a dark theme and darker on a light one, so it reads on both.
QColor InkFor(const QColor& color);
// Text over a solid fill: near-black on a light fill, white on a dark one.
QColor InkOn(const QColor& fill);

}  // namespace mira_gui
