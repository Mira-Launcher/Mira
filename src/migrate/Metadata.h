#pragma once

#include "config/Config.h"

namespace mira::migrate {

// Fetching was switched off with `metadata.enabled = false`; the switch is `metadata.disabled`
// now. Carries an old `false` over, so fetching stays off. Does nothing once settings.toml is
// written without the old key.
//
// Temporary: delete a release or two after metadata.disabled shipped.
void TurnMetadataOff(config::Config& config);

}  // namespace mira::migrate
