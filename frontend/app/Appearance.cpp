#include "Appearance.h"

#include "../theme/Theme.h"
#include "KeyBindings.h"

namespace mira_gui {

void ApplyAppearance(const FrontendPrefs& prefs) {
  // Unset leaves it to the theme; a negative value is how older builds wrote that.
  const auto shape = [](const std::optional<int>& pref) -> std::optional<int> {
    if (pref && *pref >= 0) return pref;
    return std::nullopt;
  };
  theme::Overrides overrides;
  overrides.tile_spacing = shape(prefs.tile_spacing);
  overrides.grid_margin = shape(prefs.grid_margin);
  overrides.radius_tile = shape(prefs.tile_radius);
  overrides.radius_panel = shape(prefs.panel_radius);
  overrides.radius_control = shape(prefs.control_radius);
  const QString name = QString::fromStdString(prefs.theme.value_or("auto"));
  const theme::Overrides& current = theme::CurrentOverrides();
  const bool same_shapes = current.tile_spacing == overrides.tile_spacing &&
                           current.grid_margin == overrides.grid_margin &&
                           current.radius_tile == overrides.radius_tile &&
                           current.radius_panel == overrides.radius_panel &&
                           current.radius_control == overrides.radius_control;
  if (name != theme::CurrentName() || !same_shapes) theme::Configure(name, overrides);
  keybindings::LoadOverrides(prefs.shortcut_overrides.value_or(std::map<std::string, std::string>{}));
}

}  // namespace mira_gui
