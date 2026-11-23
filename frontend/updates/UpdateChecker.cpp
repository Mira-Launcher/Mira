#include "UpdateChecker.h"

#include <algorithm>
#include <json.hpp>
#include <string>
#include <utility>

#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Library.h"

namespace mira_gui {

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (!live) return;
            const auto payload = nlohmann::json::parse(data, nullptr, false);
            if (!payload.is_object()) return;
            // Something installed may be the newer release.
            const std::string kind = payload.value("kind", std::string());
            const bool installed = type == "job.finished" &&
                                   (kind == "setup" || kind == "install" || kind == "runner");
            const bool setting_changed =
                type == "config.changed" && payload.contains("keys") &&
                std::ranges::contains(payload["keys"], nlohmann::json("updates.check"));
            if (installed || setting_changed) Check();
          });
}

void UpdateChecker::Check(bool refresh, std::function<void()> done) {
  // Kept until an answer lands, so a newer check superseding this one still runs it.
  if (done) waiting_.push_back(std::move(done));
  if (refresh) return Ask(true);
  const int request = ++request_;
  api::GetConfigAsync(this, [this, request](ConfigResult config) {
    if (request != request_) return;
    if (!config.ok) return Answered();
    const auto check = config.values.find("updates.check");
    if (check != config.values.end() && check->second == "false") {
      updates_.clear();
      emit Changed();
      return Answered();
    }
    Ask(false);
  });
}

void UpdateChecker::Answered() {
  for (const auto& done : std::exchange(waiting_, {})) done();
}

void UpdateChecker::Ask(bool refresh) {
  const int request = ++request_;
  api::GetUpdatesAsync(this, refresh, [this, request](UpdatesResult result) {
    if (request != request_) return;
    if (result.ok) {
      updates_ = std::move(result.updates);
      emit Changed();
    }
    Answered();
  });
}

}  // namespace mira_gui
