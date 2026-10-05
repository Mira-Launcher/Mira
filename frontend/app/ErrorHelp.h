#pragma once

#include <QString>

#include <functional>
#include <optional>
#include <string>

#include "../client/ApiError.h"

namespace mira_gui::error_help {

// The places an error's `fix` can send the user. Set once by the window that
// owns them; a fix whose route is unset gets no button, only its hint.
struct Navigator {
  std::function<void(const QString& key)> open_setting;
  std::function<void()> open_runners;
  std::function<void(const std::string& source)> open_source;
  std::function<void(const std::string& game_id)> open_game_settings;
  std::function<void(const std::string& game_id)> view_log;
  std::function<void(const std::string& game_id)> install_shown;  // run its installer with the window shown
  std::function<void()> start_daemon;
  std::function<QString(const std::string& source)> source_name;
};
void SetNavigator(Navigator navigator);

// A button for an error: its label and what it does.
struct Action {
  QString label;
  std::function<void()> run;
};

// The button for `error`'s fix, if it has one this window can route.
std::optional<Action> ActionFor(const ApiError& error);

// mirad's hint for `error`, or the client's own for a request that never reached it.
QString HintFor(const ApiError& error);

// One line for an inline status label: the message, then the hint.
QString Describe(const ApiError& error);

}  // namespace mira_gui::error_help
