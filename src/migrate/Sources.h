#pragma once

#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::migrate {

// Carries over what the settings and the library say is set up: stores signed in, launchers installed,
// the Steam key, sources turned off, and the sidebar's hidden and ordered sources. Runs only when this
// Load made the sources table.
//
// Temporary: delete a release or two after the sources table shipped.
void ImportSourceState(const config::Config& config, store::GameStore& games);

}  // namespace mira::migrate
