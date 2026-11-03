#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mira::config {
class Config;
class Resolver;
}  // namespace mira::config

// Ready-made steps a game can add to its pre-launch script (launch.pre_steps) and its post-exit
// script (launch.post_steps), such as going invisible on Steam and back online.
namespace mira::library {

struct ScriptStep {
  std::string id;
  std::string label;
  std::string doc;
  // Shell commands that check for their own tool and do nothing without it. {steam}, {steam_pid}
  // and {runtime} stand for the Steam command, its pid file and the socket's folder until
  // ResolveLaunchScripts.
  std::string command;
  std::string pair;  // the other phase's step that goes with this one: its undo, or its start
  bool needs_pair = false;  // does nothing unless its pair ran before the game
};

const std::vector<ScriptStep>& PreLaunchSteps();
const std::vector<ScriptStep>& PostExitSteps();

// A launch.pre_script or launch.post_script as sh -c runs it: the path of a script file runs
// that file, through sh unless it's executable; anything else is commands.
std::string OwnScript(std::string_view script);

struct LaunchScripts {
  std::string pre;
  std::string post;
};

// The pre-launch and post-exit scripts a launch runs: the game's own, then its steps. The own
// pre-launch script runs first, so a failure cancels the launch before any step starts.
LaunchScripts ResolveLaunchScripts(const config::Config& config, const config::Resolver& resolver);

}  // namespace mira::library
