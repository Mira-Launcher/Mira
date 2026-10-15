#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <array>
#include <optional>

class QWidget;

namespace mira_gui::theme {

// Every color and measurement the UI draws with. A theme file sets these and
// nothing else: the stylesheet itself is ours (:/themes/base.qss), so a theme
// cannot break when a widget is renamed, and both the QSS and the QPalette are
// generated from the same values as the custom painters read.
//
// Each field's initialiser is the fallback for a theme that omits it, which is
// what keeps a half-written file from producing an unusable window.
struct Tokens {
  QColor window{"#1b1d22"};       // the window's own backdrop
  QColor surface{"#23262d"};      // panels, top bar, inputs
  QColor surface_alt{"#2b2f37"};  // hover, alternating rows, tooltips
  QColor border{"#363b45"};
  QColor text{"#e6e8ec"};
  QColor text_muted{"#9aa0ab"};
  QColor accent{"#5c7cfa"};  // selection, focus, the active tab
  QColor on_accent{"#ffffff"};
  QColor accent_hidden{"#9b6cf5"};  // the accent in hidden mode, which also tints the top bar
  QColor running{"#43a047"};        // a game that is playing right now
  QColor success{"#43a047"};
  QColor warning{"#ef6c00"};
  QColor error{"#e5484d"};
  QColor info{"#1e88e5"};
  QColor status_ready{"#43a047"};
  QColor status_setting_up{"#1565c0"};
  QColor status_broken{"#e5484d"};
  QColor status_missing{"#757575"};
  QColor status_needs_install{"#ef6c00"};
  QColor tile_placeholder{"#303030"};

  // How dark the gradient behind a tile's title gets at the very bottom.
  int scrim_alpha = 225;

  // How dark the dim behind a modal card is.
  int modal_scrim_alpha = 150;

  int radius_panel = 8;
  int radius_control = 6;
  int radius_tile = 6;
  int radius_toast = 8;

  int tile_spacing = 5;  // gap between two tiles in the grid
  int grid_margin = 6;   // padding between the grid and the panels around it

  // Only the two sizes the UI actually names. The base size is left to the
  // desktop's own font setting, which is an accessibility setting.
  int font_size_small = 12;
  int font_size_heading = 17;

  // The saturation/value a generated placeholder cover is drawn at; the hue
  // still comes from the game's own name (see CoverArt).
  int placeholder_saturation = 110;
  int placeholder_value = 135;

  // Take the window, panel, text and accent colors from the desktop's palette, where it has one.
  // The window, panel and text colors only when the desktop is as light or dark as the theme.
  bool system_colors = false;
};

// Stand-ins for covers in a preview with no games to show: `tokens`' own
// accent colors, so they suit the theme being previewed.
inline std::array<QColor, 4> SampleArt(const Tokens& tokens) {
  return {tokens.accent, tokens.warning, tokens.success, tokens.error};
}

// `tokens` with the desktop's colors, as a theme with system_colors gets them: those of its
// palette `system` where Qt reads one (KDE), and its portal `accent` where the desktop sets one.
Tokens WithSystemColors(Tokens tokens, const std::optional<QPalette>& system,
                        std::optional<QColor> accent = std::nullopt);

// The desktop's accent and light or dark preference from its portal (theme/DesktopPortal), for
// desktops whose palette Qt doesn't read. Restyles if they change what's drawn.
void SetDesktopAppearance(std::optional<QColor> accent, std::optional<Qt::ColorScheme> scheme);

// `font` `points` larger, in whatever unit it's set in.
QFont Larger(QFont font, qreal points = 1);
// The desktop's font, which tile titles keep. Everything else is a point larger (EnlargeAppFont).
QFont DesktopFont();
// Sets the application's font a point above the desktop's, so text reads at a glance. Once, at
// start.
void EnlargeAppFont();

// What everything currently draws with. Valid before Apply() is ever called,
// it starts as the built-in defaults.
const Tokens& Current();

// The current theme's own shape values, before the user's Overrides are
// layered on: what a shape control unset (-1) actually resolves to. For
// showing that number in the settings screen instead of a placeholder like
// "Theme default".
const Tokens& ThemeDefaults();

// One user's own adjustments to the shape tokens, kept in frontend.toml
// rather than in a theme file: they are a preference about this library, not
// part of what makes a theme look like itself, and they have to survive
// switching theme. An unset field means "whatever the theme says".
struct Overrides {
  std::optional<int> tile_spacing;
  std::optional<int> grid_margin;
  std::optional<int> radius_tile;
  std::optional<int> radius_panel;
  std::optional<int> radius_control;
};

// Re-applies the current theme, so a change shows without a restart.
void SetOverrides(const Overrides& overrides);
const Overrides& CurrentOverrides();

// Writes each bundled theme into the user's theme directory unless a file of that name is there
// already, to edit or copy. A user file of a bundled theme's name applies over it key by key, so a
// key it lacks (one added in a later version) still comes from the bundled theme.
void WriteUserCopies();

// Theme names, bundled ones first, then any *.toml in the user's theme
// directory. Not including "auto", which is a resolution rule rather than a
// theme (see Apply).
QStringList Available();

// Applies a theme by name: the style, the palette and the stylesheet at once.
// "auto" follows the desktop's own light/dark preference and keeps following
// it, so changing that setting re-themes a running window.
void Apply(const QString& name);

// Hidden mode (Ctrl+H): `accent` becomes the theme's `accent_hidden` and the top bar takes its
// tint, until it's turned off again. Restyles like Apply.
void SetHiddenMode(bool on);

// SetOverrides and Apply in one restyle, for startup and a settings save
// that changes both.
void Configure(const QString& name, const Overrides& overrides);

// The name last passed to Apply(), so the settings picker can show it.
QString CurrentName();

// The colors `name` would draw with, without applying it; "auto" resolves to
// the desktop's light or dark theme. For previewing a theme before picking it.
Tokens Peek(const QString& name);

// Sets one of the stylesheet's own properties on a widget: "role" for a text
// style (muted, heading, section, error, keys), "status" for a lifecycle
// color. Qt does not restyle a widget when a property changes after it has
// been polished, so this re-polishes it; setting the property directly works
// only before the widget is first shown.
void SetStyleProperty(QWidget* widget, const char* name, const QString& value);

// Changed() fires after every Apply(). Anything that paints with tokens
// instead of with the stylesheet (the tile delegate, the toasts) connects to
// it and repaints.
class Notifier : public QObject {
  Q_OBJECT

 public:
  static Notifier* Instance();

 signals:
  void Changed();
};

// A QColor as CSS: hex when opaque, rgba() otherwise.
QString ColorToQss(const QColor& color);

// Sets a widget's own style sheet unless it already has that one. Qt repolishes the widget on
// every call, the same sheet included, so this is for code that runs again and again.
void SetStyleSheet(QWidget* widget, const QString& sheet);

}  // namespace mira_gui::theme
