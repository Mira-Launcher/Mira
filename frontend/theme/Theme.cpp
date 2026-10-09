#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QWidget>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <toml.hpp>

namespace mira_gui::theme {
namespace {

Tokens g_tokens;
Tokens g_theme_tokens;  // parsed theme, before g_overrides is layered on
Overrides g_overrides;
QString g_name = "auto";
QString g_resolved = "mira-dark";
bool g_following_desktop = false;

// "#rrggbb" and "#rrggbbaa", in CSS order, because that is the order anyone
// writing a theme by hand will reach for. (QColor's own "#aarrggbb" puts
// alpha first, which silently turns a translucent panel into a tinted one.)
QColor ParseColor(const std::string& text, const QColor& fallback) {
  const QString value = QString::fromStdString(text).trimmed();
  if (value.size() == 9 && value.startsWith('#')) {
    bool ok = false;
    const uint rgba = value.mid(1).toUInt(&ok, 16);
    if (!ok) return fallback;
    return QColor((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
  }
  const QColor parsed(value);
  return parsed.isValid() ? parsed : fallback;
}

// Where a user's own themes live, following the same env-var-then-XDG rule as
// the socket path (client/Transport.cpp).
std::filesystem::path UserThemeDir() {
  const char* config_home = std::getenv("XDG_CONFIG_HOME");
  if (config_home != nullptr && *config_home != '\0') {
    return std::filesystem::path(config_home) / "mira" / "themes";
  }
  const char* home = std::getenv("HOME");
  const std::filesystem::path base = home != nullptr && *home != '\0' ? home : ".";
  return base / ".config" / "mira" / "themes";
}

QString BundledPath(const QString& name) { return QString(":/themes/%1.toml").arg(name); }

// A theme file, bundled or not, as text. Empty if it isn't there.
std::string ReadThemeFile(const QString& name) {
  const QString bundled = BundledPath(name);
  if (QFile::exists(bundled)) {
    QFile file(bundled);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return file.readAll().toStdString();
    }
  }
  const std::filesystem::path user = UserThemeDir() / (name.toStdString() + ".toml");
  QFile file(QString::fromStdString(user.string()));
  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) return file.readAll().toStdString();
  return {};
}

// Every key is optional: a value that is missing, misspelled or of the wrong
// type leaves that token at its default rather than failing the whole file, so
// a half-written theme still produces a usable window.
Tokens ParseTokens(const std::string& text) {
  Tokens tokens;
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) return tokens;
  const toml::table& table = parsed.table();

  auto color = [&table](const char* key, QColor& out) {
    if (const auto value = table[key].value<std::string>()) out = ParseColor(*value, out);
  };
  auto number = [&table](const char* key, int& out) {
    if (const auto value = table[key].value<int64_t>()) out = static_cast<int>(*value);
  };

  color("window", tokens.window);
  color("surface", tokens.surface);
  color("surface_alt", tokens.surface_alt);
  color("border", tokens.border);
  color("text", tokens.text);
  color("text_muted", tokens.text_muted);
  color("accent", tokens.accent);
  color("on_accent", tokens.on_accent);
  color("running", tokens.running);
  color("success", tokens.success);
  color("warning", tokens.warning);
  color("error", tokens.error);
  color("info", tokens.info);
  color("status_ready", tokens.status_ready);
  color("status_setting_up", tokens.status_setting_up);
  color("status_broken", tokens.status_broken);
  color("status_missing", tokens.status_missing);
  color("status_needs_install", tokens.status_needs_install);
  color("tile_placeholder", tokens.tile_placeholder);

  number("scrim_alpha", tokens.scrim_alpha);
  number("modal_scrim_alpha", tokens.modal_scrim_alpha);
  number("radius_panel", tokens.radius_panel);
  number("radius_control", tokens.radius_control);
  number("radius_tile", tokens.radius_tile);
  number("radius_toast", tokens.radius_toast);
  number("font_size_small", tokens.font_size_small);
  number("font_size_heading", tokens.font_size_heading);
  number("placeholder_saturation", tokens.placeholder_saturation);
  number("placeholder_value", tokens.placeholder_value);
  number("tile_spacing", tokens.tile_spacing);
  number("grid_margin", tokens.grid_margin);

  return tokens;
}

// A checkbox's tick and a combo's chevron cannot be shapes in the stylesheet,
// Qt takes only an image for a sub-control like that. They are drawn here, in
// the theme's own colors, and cached as PNG rather than SVG so nothing depends
// on the qtsvg image plugin being present (it is not something the AppImage can
// count on bundling).
QString GlyphPath(const QString& kind, const QColor& color) {
  const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/glyphs";
  QDir().mkpath(dir);
  // The color is in the name because Qt caches an image by its path, and two
  // themes' chevrons would otherwise be the same file.
  const QString path = QString("%1/%2-%3.png").arg(dir, kind, color.name(QColor::HexArgb).mid(1));
  if (QFile::exists(path)) return path;

  constexpr int kSize = 32;  // 2x the 16px the stylesheet asks for
  QImage image(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(color, kind == "check" ? 4.0 : 3.2, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
  QPainterPath stroke;
  if (kind == "check") {
    stroke.moveTo(8, 17);
    stroke.lineTo(13.5, 22.5);
    stroke.lineTo(24, 10);
  } else if (kind == "chevron_up") {
    stroke.moveTo(9, 20);
    stroke.lineTo(16, 13);
    stroke.lineTo(23, 20);
  } else {
    stroke.moveTo(9, 13);
    stroke.lineTo(16, 20);
    stroke.lineTo(23, 13);
  }
  painter.drawPath(stroke);
  painter.end();
  return image.save(path, "PNG") ? path : QString();
}

QHash<QString, QString> QssValues(const Tokens& tokens) {
  QHash<QString, QString> values{
      {"window", ColorToQss(tokens.window)},
      {"surface", ColorToQss(tokens.surface)},
      {"surface_alt", ColorToQss(tokens.surface_alt)},
      {"border", ColorToQss(tokens.border)},
      {"text", ColorToQss(tokens.text)},
      {"text_muted", ColorToQss(tokens.text_muted)},
      {"accent", ColorToQss(tokens.accent)},
      {"on_accent", ColorToQss(tokens.on_accent)},
      {"running", ColorToQss(tokens.running)},
      {"success", ColorToQss(tokens.success)},
      {"warning", ColorToQss(tokens.warning)},
      {"error", ColorToQss(tokens.error)},
      {"info", ColorToQss(tokens.info)},
  };
  QColor accent_soft = tokens.accent;
  accent_soft.setAlphaF(0.3);
  values.insert("accent_soft", ColorToQss(accent_soft));
  // Faint washes behind a note or a mark, still readable under text.
  for (const auto& [name, color] : {std::pair{"accent_faint", tokens.accent},
                                    std::pair{"info_faint", tokens.info},
                                    std::pair{"success_faint", tokens.success}}) {
    QColor faint = color;
    faint.setAlphaF(0.14);
    values.insert(name, ColorToQss(faint));
  }
  values.insert("radius_panel", QString("%1px").arg(tokens.radius_panel));
  values.insert("radius_control", QString("%1px").arg(tokens.radius_control));
  values.insert("font_size_small", QString("%1px").arg(tokens.font_size_small));
  values.insert("font_size_heading", QString("%1px").arg(tokens.font_size_heading));
  values.insert("check_glyph", QString("url(%1)").arg(GlyphPath("check", tokens.on_accent)));
  values.insert("chevron_glyph", QString("url(%1)").arg(GlyphPath("chevron", tokens.text_muted)));
  values.insert("chevron_up_glyph",
                QString("url(%1)").arg(GlyphPath("chevron_up", tokens.text_muted)));
  return values;
}

// Substituted by name rather than by search-and-replace: replacing "@text"
// textually would also eat the "@text_muted" occurrences, and an unknown token
// should be visible as an empty declaration rather than silently left as
// literal "@nonsense" in the stylesheet.
QString RenderQss(const Tokens& tokens) {
  QFile file(":/themes/base.qss");
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QString sheet = QString::fromUtf8(file.readAll());

  const QHash<QString, QString> values = QssValues(tokens);
  static const QRegularExpression placeholder("@([a-z_]+)");
  QString out;
  out.reserve(sheet.size());
  qsizetype last = 0;
  QRegularExpressionMatchIterator matches = placeholder.globalMatch(sheet);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    out += sheet.mid(last, match.capturedStart() - last);
    out += values.value(match.captured(1));
    last = match.capturedEnd();
  }
  out += sheet.mid(last);
  return out;
}

QPalette PaletteFor(const Tokens& tokens) {
  QPalette palette;
  palette.setColor(QPalette::Window, tokens.window);
  palette.setColor(QPalette::WindowText, tokens.text);
  palette.setColor(QPalette::Base, tokens.surface);
  palette.setColor(QPalette::AlternateBase, tokens.surface_alt);
  palette.setColor(QPalette::Text, tokens.text);
  palette.setColor(QPalette::PlaceholderText, tokens.text_muted);
  palette.setColor(QPalette::Button, tokens.surface);
  palette.setColor(QPalette::ButtonText, tokens.text);
  palette.setColor(QPalette::ToolTipBase, tokens.surface_alt);
  palette.setColor(QPalette::ToolTipText, tokens.text);
  palette.setColor(QPalette::Highlight, tokens.accent);
  palette.setColor(QPalette::HighlightedText, tokens.on_accent);
  palette.setColor(QPalette::Link, tokens.accent);
  palette.setColor(QPalette::BrightText, tokens.error);
  palette.setColor(QPalette::Disabled, QPalette::Text, tokens.text_muted);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, tokens.text_muted);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, tokens.text_muted);
  return palette;
}

QString DesktopTheme() {
  return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light ? "mira-light"
                                                                               : "mira-dark";
}

void ApplyResolved(const QString& resolved) {
  g_resolved = resolved;
  const std::string text = ReadThemeFile(resolved);
  g_tokens = text.empty() ? Tokens{} : ParseTokens(text);
  g_theme_tokens = g_tokens;

  // Last word: a theme supplies the shape, the user's own adjustment wins
  // over it.
  if (g_overrides.tile_spacing) g_tokens.tile_spacing = *g_overrides.tile_spacing;
  if (g_overrides.grid_margin) g_tokens.grid_margin = *g_overrides.grid_margin;
  if (g_overrides.radius_tile) g_tokens.radius_tile = *g_overrides.radius_tile;
  if (g_overrides.radius_panel) g_tokens.radius_panel = *g_overrides.radius_panel;
  if (g_overrides.radius_control) g_tokens.radius_control = *g_overrides.radius_control;

  // Fusion rather than the desktop's own style: ours is the only palette and
  // stylesheet in play, and Breeze/Adwaita would otherwise keep drawing the
  // parts our QSS doesn't name, which is what "half-themed" looks like.
  if (QApplication::style() == nullptr || QApplication::style()->name() != "fusion") {
    QApplication::setStyle(QStyleFactory::create("Fusion"));
  }
  QApplication::setPalette(PaletteFor(g_tokens));
  qApp->setStyleSheet(RenderQss(g_tokens));
  emit Notifier::Instance()->Changed();
}

}  // namespace

QString ColorToQss(const QColor& color) {
  if (color.alpha() == 255) return color.name(QColor::HexRgb);
  return QString("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alphaF(), 0, 'f', 3);
}

const Tokens& Current() { return g_tokens; }

const Tokens& ThemeDefaults() { return g_theme_tokens; }

const Overrides& CurrentOverrides() { return g_overrides; }

void SetOverrides(const Overrides& overrides) {
  g_overrides = overrides;
  ApplyResolved(g_resolved);
}

void SetStyleProperty(QWidget* widget, const char* name, const QString& value) {
  if (widget == nullptr) return;
  if (widget->property(name).toString() == value) return;
  widget->setProperty(name, value);
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

QString CurrentName() { return g_name; }

Tokens Peek(const QString& name) {
  const std::string text = ReadThemeFile(name == "auto" ? DesktopTheme() : name);
  return text.empty() ? Tokens{} : ParseTokens(text);
}

Notifier* Notifier::Instance() {
  static Notifier instance;
  return &instance;
}

QStringList Available() {
  QStringList names{"mira-dark", "mira-light"};
  QDir dir(QString::fromStdString(UserThemeDir().string()));
  for (const QFileInfo& info : dir.entryInfoList({"*.toml"}, QDir::Files, QDir::Name)) {
    const QString name = info.completeBaseName();
    if (!names.contains(name)) names.push_back(name);
  }
  return names;
}

void Apply(const QString& name) {
  g_name = name.isEmpty() ? "auto" : name;

  if (g_name == "auto") {
    ApplyResolved(DesktopTheme());
    if (!g_following_desktop) {
      g_following_desktop = true;
      QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                       Notifier::Instance(), [] {
                         if (g_name == "auto") ApplyResolved(DesktopTheme());
                       });
    }
    return;
  }
  ApplyResolved(g_name);
}

void Configure(const QString& name, const Overrides& overrides) {
  g_overrides = overrides;
  Apply(name);
}

}  // namespace mira_gui::theme
