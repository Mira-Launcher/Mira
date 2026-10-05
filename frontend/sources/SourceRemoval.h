#pragma once

#include <functional>

#include "Sources.h"

class QWidget;

namespace mira_gui {

// Shows what removing `source` would do, asks, then removes it.
// `on_removed` runs only once it's gone.
void RemoveSource(QWidget* parent, const SourceInfo& source, std::function<void()> on_removed);

}  // namespace mira_gui
