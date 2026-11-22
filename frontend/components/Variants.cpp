#include "Variants.h"

#include <algorithm>
#include <cmath>

#include "../theme/Theme.h"

namespace mira_gui {

QColor ToneColor(Tone tone) {
  const theme::Tokens& tokens = theme::Current();
  switch (tone) {
    case Tone::Default:
      return tokens.text;
    case Tone::Muted:
      return tokens.text_muted;
    case Tone::Accent:
      return tokens.accent;
    case Tone::Info:
      return tokens.info;
    case Tone::Success:
      return tokens.success;
    case Tone::Warning:
      return tokens.warning;
    case Tone::Danger:
      return tokens.error;
  }
  return tokens.text;
}

QColor InkFor(const QColor& color) {
  // Only the lightness moves, so a tier keeps its hue: ProtonDB's dark "native" green lifts on a
  // dark theme and its pale platinum darkens on a light one.
  const bool dark_theme = theme::Current().surface.lightnessF() < 0.5;
  const float lightness = color.lightnessF();
  return QColor::fromHslF(color.hslHueF(), color.hslSaturationF(),
                          dark_theme ? std::max(lightness, 0.62f) : std::min(lightness, 0.38f));
}

QColor InkOn(const QColor& fill) {
  const auto linear = [](double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  const double luminance =
      0.2126 * linear(fill.redF()) + 0.7152 * linear(fill.greenF()) + 0.0722 * linear(fill.blueF());
  // Past this, black text has more contrast than white.
  return luminance > 0.179 ? QColor(20, 22, 26) : QColor(Qt::white);
}

}  // namespace mira_gui
