#pragma once

#include "../client/Types.h"

namespace mira_gui {

// Theme, shape overrides and shortcut overrides from frontend.toml. Restyles
// only when the theme or a shape actually differs from what's applied.
void ApplyAppearance(const FrontendPrefs& prefs);

}  // namespace mira_gui
