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
  Write(dark, "accent = \"#123456\"\n");
  theme::WriteUserCopies();
  const theme::Tokens edited = theme::Peek("mira-dark");
  CHECK(edited.accent == QColor("#123456"));
  CHECK(edited.window == built_in.window);
  CHECK(edited.accent_hidden == built_in.accent_hidden);
}
