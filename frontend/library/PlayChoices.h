#pragma once

#include <QString>
#include <string>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// One thing to start: a launch option's id ("game" for the game's own program) and its label.
struct PlayChoice {
  std::string option;
  QString label;
};

// What Play offers: the default first, then the game's own program and its other play options. Only
// the default when its launch says so, unless `all`.
std::vector<PlayChoice> PlayChoices(const GameSummary& game, bool all);
// Its tool options, which open by themselves.
std::vector<PlayChoice> ToolChoices(const GameSummary& game);

}  // namespace mira_gui
