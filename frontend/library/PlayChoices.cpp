#include "PlayChoices.h"

#include <algorithm>

namespace mira_gui {

std::vector<PlayChoice> PlayChoices(const GameSummary& game, bool all) {
  std::vector<PlayChoice> choices = {{"game", RunVerb(game)}};
  for (const LaunchOption& option : game.launch.options) {
    if (option.mode == "play") choices.push_back({option.id, QString::fromStdString(option.name)});
  }
  const auto chosen = std::ranges::find(choices, game.launch.default_id, &PlayChoice::option);
  if (game.launch.default_id.empty() || chosen == choices.end()) return choices;
  std::rotate(choices.begin(), chosen, chosen + 1);
  choices[1].label = "Play directly";  // the game's own program, now behind the default
  if (game.launch.only && !all) choices.resize(1);
  return choices;
}

std::vector<PlayChoice> ToolChoices(const GameSummary& game) {
  std::vector<PlayChoice> tools;
  for (const LaunchOption& option : game.launch.options) {
    if (option.mode == "tool") tools.push_back({option.id, QString::fromStdString(option.name)});
  }
  return tools;
}

}  // namespace mira_gui
