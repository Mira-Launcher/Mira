#pragma once

#include <QObject>
#include <optional>
#include <cstdint>
#include <functional>
#include <string>

#include "../Types.h"

namespace mira_gui::api {

// GET /v1/logs/<channel>: what a task has logged since `after` (the `next` of
// the last answer), or its last `lines` lines with no cursor. Channels are
// daemon, game:<id>, setup:<source>, install:<source>:<ref>, runner:<kind>:<name>.
void GetLogAsync(QObject* context, const std::string& channel, const std::optional<std::uint64_t>& after,
                 int lines, std::function<void(LogResult)> callback);

}  // namespace mira_gui::api
