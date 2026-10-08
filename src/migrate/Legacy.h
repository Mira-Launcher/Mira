#pragma once

#include "store/GameStore.h"

namespace mira::migrate {

// Moves what Mira 0.13 and earlier kept in files into the databases, once:
// games.toml, metadata/*.json, sessions/*.toml and the window state in
// frontend.toml. Each source is renamed or removed once it's in.
//
// Temporary. To drop it: delete src/migrate/, its call in mirad_main.cpp and
// tests/legacy_import_test.cpp.
void ImportLegacyFiles(store::GameStore& games);

}  // namespace mira::migrate
