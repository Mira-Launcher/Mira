#include <doctest.h>

#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <optional>
#include <string>

#include "theme/Theme.h"

using namespace mira_gui;

namespace {
// Points XDG_CONFIG_HOME at a scratch folder for the test's lifetime.
class ScratchConfig {
public:
  ScratchConfig() {
    if (const char* old = std::getenv("XDG_CONFIG_HOME")) old_ = old;
    ::setenv("XDG_CONFIG_HOME", dir_.path().toLocal8Bit().constData(), 1);
  }
  ~ScratchConfig() {
    if (old_) {
      ::setenv("XDG_CONFIG_HOME", old_->c_str(), 1);
    } else {
      ::unsetenv("XDG_CONFIG_HOME");
    }
  }
  QString Themes() const { return dir_.path() + "/mira/themes"; }

private:
  QTemporaryDir dir_;
  std::optional<std::string> old_;
};

void Write(const QString& path, const QByteArray& text) {
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(text);
}
}  // namespace

TEST_CASE("The built-in themes are written out to edit, and a user's lines apply over them") {
  ScratchConfig config;
  const theme::Tokens built_in = theme::Peek("mira-dark");

  theme::WriteUserCopies();
  const QString dark = config.Themes() + "/mira-dark.toml";
  REQUIRE(QFile::exists(dark));
  REQUIRE(QFile::exists(config.Themes() + "/mira-light.toml"));
  // The copy defines the theme as it is.
  CHECK(theme::Peek("mira-dark").accent == built_in.accent);
  CHECK(theme::Peek("mira-dark").window == built_in.window);

  // An edited file wins key by key, a key it leaves out stays built in, and a second start keeps
  // it.
  Write(dark, "system_colors = false\naccent = \"#123456\"\n");
  theme::WriteUserCopies();
  const theme::Tokens edited = theme::Peek("mira-dark");
  CHECK(edited.accent == QColor("#123456"));
  CHECK(edited.window == built_in.window);
  CHECK(edited.accent_hidden == built_in.accent_hidden);
}

TEST_CASE(
    "A theme takes the desktop's colors, and only its accent when the desktop is the other "
    "brightness") {
  theme::Tokens dark;
  dark.window = QColor("#16181d");
  dark.text = QColor("#e6e8ec");
  dark.accent = QColor("#5c7cfa");
  theme::Tokens light = dark;
  light.window = QColor("#f4f5f7");
  light.text = QColor("#1c1f25");

  QPalette desktop;
  desktop.setColor(QPalette::Window, QColor(28, 28, 33));
  desktop.setColor(QPalette::WindowText, QColor(252, 252, 252));
  desktop.setColor(QPalette::Base, QColor(16, 16, 19));
  desktop.setColor(QPalette::Highlight, QColor(4, 152, 134));
  desktop.setColor(QPalette::HighlightedText, QColor(255, 255, 255));

  const theme::Tokens taken = theme::WithSystemColors(dark, desktop);
  CHECK(taken.window == QColor(28, 28, 33));
  CHECK(taken.text == QColor(252, 252, 252));
  CHECK(taken.accent == QColor(4, 152, 134));
  CHECK(taken.on_accent == QColor(255, 255, 255));
  // Panels stay a step above the window even where the desktop's view color is below it.
  CHECK(taken.surface.lightness() > taken.window.lightness());
  CHECK(taken.surface_alt.lightness() > taken.surface.lightness());

  const theme::Tokens kept = theme::WithSystemColors(light, desktop);
  CHECK(kept.window == light.window);
  CHECK(kept.text == light.text);
  CHECK(kept.accent == QColor(4, 152, 134));

  // A desktop with only a portal accent (GNOME) keeps the theme's own colors around it, and a
  // light accent gets dark text.
  const theme::Tokens accented = theme::WithSystemColors(dark, std::nullopt, QColor(53, 132, 228));
  CHECK(accented.window == dark.window);
  CHECK(accented.accent == QColor(53, 132, 228));
  CHECK(accented.on_accent.lightness() > 200);
  const theme::Tokens yellow = theme::WithSystemColors(dark, std::nullopt, QColor(245, 194, 17));
  CHECK(yellow.on_accent.lightness() < 60);
  // Where the palette's highlight is the portal's accent, its own text color stays.
  const theme::Tokens both = theme::WithSystemColors(dark, desktop, QColor(4, 152, 134));
  CHECK(both.on_accent == QColor(255, 255, 255));
}
