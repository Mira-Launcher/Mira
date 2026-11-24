#pragma once

#include <QString>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// Where a game card's cards go: two columns, how the game runs on the left and the game itself on
// the right, each a list of card titles in order. The one place to change their order. A card for
// a settings group not listed here (one mirad adds later) goes at the end of the right column.
struct CardColumns {
  std::vector<QString> runs;  // "How it runs"
  std::vector<QString> game;  // "The game"
};

// The kind of game decides which cards it has: a Steam game has Steam's card and no runner or
// executable, an emulator's game its emulator's, and any other a Runner card, with its store's card
// (titled `store`, empty for none) after it, and a Launch card on the right.
CardColumns CardColumnsFor(const GameDetail& game, const QString& store = {});

// Whether a game has a prefix and data folder row: a Windows game whose files Mira looks after
// (not a desktop entry's, nor an HTML game's, whose data folder is its browser profile), or a
// Steam game that already has one.
bool ShowsPrefixRow(const GameDetail& game);

}  // namespace mira_gui
