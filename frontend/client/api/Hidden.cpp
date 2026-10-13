#include "Hidden.h"

#include <json.hpp>
#include <utility>

#include "../Async.h"
#include "../Transport.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

HiddenState StateOf(const transport::Reply& reply) {
  HiddenState state;
  state.ok = reply.ok && reply.body.is_object();
  if (!reply.ok) {
    state.error = reply.error;
    return state;
  }
  state.password = reply.body.value("password", false);
  state.locked = reply.body.value("locked", false);
  return state;
}

}  // namespace

void GetHiddenStateAsync(QObject* context, std::function<void(HiddenState)> callback) {
  async::Run(context, [] { return StateOf(transport::Get("/v1/hidden")); }, std::move(callback));
}

void UnlockHiddenAsync(QObject* context, const std::string& password,
                       std::function<void(HiddenState)> callback) {
  async::Run(
      context,
      [password] {
        return StateOf(transport::PostJson("/v1/hidden/unlock", {{"password", password}}));
      },
      std::move(callback));
}

void LeftHiddenModeAsync(QObject* context) {
  async::Run(
      context, [] { return transport::Post("/v1/hidden/left").ok; },
      std::function<void(bool)>([](bool) {}));
}

void LockHiddenNow() {
  transport::Post("/v1/hidden/lock");
}

void SetHiddenPasswordAsync(QObject* context, const std::string& current, const std::string& next,
                            std::function<void(HiddenState)> callback) {
  async::Run(
      context,
      [current, next] {
        const json body = {{"current", current}, {"password", next}};
        return StateOf(transport::Put("/v1/hidden/password", body.dump(), "application/json"));
      },
      std::move(callback));
}

void NoteHiddenActivityAsync(QObject* context) {
  async::Run(
      context, [] { return transport::Post("/v1/hidden/activity").ok; },
      std::function<void(bool)>([](bool) {}));
}

}  // namespace mira_gui::api
