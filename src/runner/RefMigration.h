#pragma once

#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::runner {

// Builds updated in place (a distro package, Steam's own Proton) are named by
// their folder. References written before that named the release inside
// ("proton:cachyos-11.0-20260703-slr"), which stops existing on the next
// update; this points them at the folder's build, when exactly one fits
// (same major version, unless the folder isn't versioned). Covers games' runner_ref,
// their overrides and every runner setting. Returns how many it changed.
int MigrateInPlaceRefs(config::Config& config, store::GameStore& games);

}  // namespace mira::runner
