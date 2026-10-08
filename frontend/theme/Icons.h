#pragma once

#include <QIcon>

#include "Theme.h"

class QAbstractButton;
class QAction;
class QLabel;

namespace mira_gui::icons {

// Every glyph the top bar and sidebar draw. Not QStyle::standardIcon --
// under Fusion those are dated 3D titlebar buttons in the platform's color,
// not the theme's.
enum class Glyph {
  Menu,
  Settings,
  Minimize,
  Maximize,
  Restore,
  Close,
  Home,
  Plus,
  Search,
  Filter,
  SortArrows,
  ChevronDown,
  Play,
  CheckCircle,
  Download,
  Clock,
  Warning,
  CircleX,
  Moon,
  EyeSlash,
  Wrench,
  Image,
  Trash,
  Refresh,
  Keyboard,
  Info,
  Store,
  Sliders,
  Dot,
  Pin,
  Monitor,
  Sidebar,
  Folder,
  Layers,
  Target,
  Grid,
  ArrowLeft,
  Undo,
  Grip,
  More,
  Eye,
  External,
  Tag,
};

// Drawn on demand in the current theme's text color, at the handful of sizes
// Qt might ask for. The color is baked in, so a widget that outlives a theme
// change uses Follow below, or redraws on theme::Notifier::Changed itself.
QIcon For(Glyph glyph);

// Same, in a caller-chosen color, such as a muted row or an on_accent icon,
// neither the plain text color the no-argument overload assumes.
QIcon For(Glyph glyph, const QColor& color);

// Sets the icon in one of the theme's colors now and again on every theme
// change, for as long as the widget lives. A fixed color (white on art) uses
// For(glyph, color) instead, since it never needs redrawing.
using Role = QColor theme::Tokens::*;
void Follow(QAbstractButton* button, Glyph glyph, Role role = &theme::Tokens::text);
void Follow(QAction* action, Glyph glyph, Role role = &theme::Tokens::text);
// A label showing the glyph as a `size` px pixmap.
void Follow(QLabel* label, Glyph glyph, int size, Role role = &theme::Tokens::text_muted);

}  // namespace mira_gui::icons
