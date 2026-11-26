#include "migrate/Metadata.h"

#include "core/Log.h"

namespace mira::migrate {

void TurnMetadataOff(config::Config& config) {
  // `metadata.enabled` is no setting now, so Load dropped it from the document.
  const nlohmann::json document = config.FileAsLoaded();
  const auto section = document.find("metadata");
  if (section == document.end() || !section->is_object()) return;
  const auto enabled = section->find("enabled");
  if (enabled == section->end() || *enabled != false) return;
  if (auto set = config.Set("metadata.disabled", true); !set) {
    log::Warn("could not keep fetching off: {}", set.error().message);
  }
}

}  // namespace mira::migrate
