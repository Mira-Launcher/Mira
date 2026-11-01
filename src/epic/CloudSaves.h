#pragma once

#include "saves/Cloud.h"

namespace mira::epic {

// Epic's cloud saves, through legendary: where they go from `legendary info`'s cloud save folder,
// the cloud's newest save from `list-saves`, and the files moved by `sync-saves` with Mira's
// resolved folder as --save-path and a forced direction, so legendary never guesses a prefix or
// decides itself.
const saves::CloudSide& Cloud();

}  // namespace mira::epic
