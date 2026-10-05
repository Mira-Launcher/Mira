#include "ErrorHelp.h"

namespace mira_gui::error_help {
namespace {

Navigator& Nav() {
  static Navigator navigator;
  return navigator;
}

}  // namespace

void SetNavigator(Navigator navigator) { Nav() = std::move(navigator); }

std::optional<Action> ActionFor(const ApiError& error) {
  const Navigator& nav = Nav();
  const ErrorFix& fix = error.fix;
  if (error.code == ApiError::kUnreachable && nav.start_daemon) return Action{"Start mirad", nav.start_daemon};
  if (fix.kind == "setting" && nav.open_setting) {
    return Action{"Open settings", [key = QString::fromStdString(fix.target)] { Nav().open_setting(key); }};
  }
  if (fix.kind == "runners" && nav.open_runners) return Action{"Open Runners", nav.open_runners};
  if (fix.kind == "source" && nav.open_source) {
    const QString name = nav.source_name ? nav.source_name(fix.target) : QString();
    if (name.isEmpty()) return std::nullopt;
    return Action{"Open " + name, [source = fix.target] { Nav().open_source(source); }};
  }
  if (fix.kind == "game" && fix.step == "log" && nav.view_log) {
    return Action{"View log", [id = fix.target] { Nav().view_log(id); }};
  }
  if (fix.kind == "game" && fix.step == "install" && nav.install_shown) {
    return Action{"Show the installer", [id = fix.target] { Nav().install_shown(id); }};
  }
  if (fix.kind == "game" && nav.open_game_settings) {
    return Action{"Game settings", [id = fix.target] { Nav().open_game_settings(id); }};
  }
  return std::nullopt;
}

QString HintFor(const ApiError& error) {
  if (error.code == ApiError::kUnreachable) {
    return "Mira's background service (mirad) isn't running. If it keeps stopping, run \"journalctl --user "
           "-u mirad\" or start \"mirad\" in a terminal to see why.";
  }
  return QString::fromStdString(error.hint);
}

QString Describe(const ApiError& error) {
  QString text = QString::fromStdString(error.message);
  const QString hint = HintFor(error);
  if (hint.isEmpty()) return text;
  if (!text.isEmpty() && !text.endsWith('.')) text += '.';
  return text.isEmpty() ? hint : text + ' ' + hint;
}

}  // namespace mira_gui::error_help
