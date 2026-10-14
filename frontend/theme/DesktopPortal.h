#pragma once

namespace mira_gui::theme::portal {

// Reads the desktop's accent color and light or dark preference from xdg-desktop-portal's
// appearance settings (GNOME, Plasma, Cinnamon and others), then follows their changes. Without
// a portal, or settings left unset, Mira keeps its own. Call once, before the first theme Apply.
void Watch();

}  // namespace mira_gui::theme::portal
