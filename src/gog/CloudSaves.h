#pragma once

#include "saves/Cloud.h"

namespace mira::gog {

// GOG's cloud saves, as Galaxy keeps them. Where they go comes from GOG's remote config for the
// game's client id; what's there from its cloudstorage listing, with a token for that client
// that gogdl gets; the files move through `gogdl save-sync`, always with a forced direction.
// Endpoints: $MIRA_GOG_CONTENT_URL, $MIRA_GOG_REMOTE_CONFIG_URL, $MIRA_GOG_CLOUD_URL.
const saves::CloudSide& Cloud();

}  // namespace mira::gog
