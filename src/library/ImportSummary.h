#pragma once

#include <vector>

#include "model/Types.h"

namespace mira::library {

// What importing a store's installed games did.
struct ImportSummary {
  int added = 0;
  int updated = 0;

  // The newly-added games; see ScanSummary::added_games in library/Scanner.h
  // for why a metadata fetch isn't triggered in the importer.
  std::vector<model::Game> added_games;
};

}  // namespace mira::library
