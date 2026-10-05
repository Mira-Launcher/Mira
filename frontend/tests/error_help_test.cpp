#include <doctest.h>

#include <string>

#include "ui/ErrorHelp.h"

using namespace mira_gui;

namespace {

ApiError WithFix(const std::string& kind, const std::string& target, const std::string& step = "") {
  ApiError error;
  error.code = "x";
  error.fix = {.kind = kind, .target = target, .step = step};
  return error;
}

}  // namespace

TEST_CASE("An error's fix button opens the place that fixes it, and none appears without a route") {
  std::string opened;
  error_help::Navigator nav;
  nav.open_setting = [&](const QString& key) { opened = "setting " + key.toStdString(); };
  nav.open_source = [&](const std::string& source) { opened = "source " + source; };
  nav.source_name = [](const std::string& source) {
    return source == "gog" ? QString("GOG") : QString();
  };
  nav.view_log = [&](const std::string& id) { opened = "log " + id; };
  nav.install_shown = [&](const std::string& id) { opened = "installer " + id; };
  nav.open_game_settings = [&](const std::string& id) { opened = "game " + id; };
  error_help::SetNavigator(nav);

  const auto press = [&](const ApiError& error) {
    opened.clear();
    const auto action = error_help::ActionFor(error);
    if (action) action->run();
    return opened;
  };
  CHECK(press(WithFix("setting", "steamgriddb.api_key")) == "setting steamgriddb.api_key");
  CHECK(press(WithFix("source", "gog", "login")) == "source gog");
  CHECK(press(WithFix("game", "celeste", "log")) == "log celeste");
  CHECK(press(WithFix("game", "celeste", "install")) == "installer celeste");
  CHECK(press(WithFix("game", "celeste", "exe")) == "game celeste");

  // A source the window can't name, a route it doesn't have, and no fix at all.
  CHECK_FALSE(error_help::ActionFor(WithFix("source", "mystery")));
  CHECK_FALSE(error_help::ActionFor(WithFix("runners", "")));
  CHECK_FALSE(error_help::ActionFor(ApiError{}));

  // mirad unreachable: starting it is the fix, once the window can.
  ApiError unreachable;
  unreachable.code = ApiError::kUnreachable;
  CHECK_FALSE(error_help::ActionFor(unreachable));
  bool started = false;
  nav.start_daemon = [&] { started = true; };
  error_help::SetNavigator(nav);
  error_help::ActionFor(unreachable)->run();
  CHECK(started);
  CHECK_FALSE(error_help::HintFor(unreachable).isEmpty());

  error_help::SetNavigator({});
}

TEST_CASE("An error reads as its message then mirad's hint") {
  ApiError error;
  error.message = "no cover found";
  error.hint = "Add a SteamGridDB key.";
  CHECK(error_help::Describe(error) == "no cover found. Add a SteamGridDB key.");
  error.hint.clear();
  CHECK(error_help::Describe(error) == "no cover found");
  error.message.clear();
  error.hint = "Add a SteamGridDB key.";
  CHECK(error_help::Describe(error) == "Add a SteamGridDB key.");
}
