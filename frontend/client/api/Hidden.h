#pragma once

#include <QObject>
#include <functional>
#include <string>

#include "../ApiError.h"

// The password on hidden games (docs/api.md#hidden-games). Each call runs on a worker thread and
// delivers its result on the main thread (client/Async.h).
namespace mira_gui::api {

struct HiddenState {
  bool ok = false;
  bool password = false;  // one is set
  bool locked = false;    // mirad leaves hidden games out of everything until it's given
  ApiError error;         // code "wrong_password" for a wrong one
};

// GET /v1/hidden.
void GetHiddenStateAsync(QObject* context, std::function<void(HiddenState)> callback);
// POST /v1/hidden/unlock.
void UnlockHiddenAsync(QObject* context, const std::string& password,
                       std::function<void(HiddenState)> callback);
// POST /v1/hidden/left: locks them when the hidden.lock_on_leave setting is on.
void LeftHiddenModeAsync(QObject* context);
// POST /v1/hidden/lock, waiting for the answer: for quitting, when no event loop is left to deliver
// one.
void LockHiddenNow();
// PUT /v1/hidden/password: sets, changes or (with `next` empty) removes it.
void SetHiddenPasswordAsync(QObject* context, const std::string& current, const std::string& next,
                            std::function<void(HiddenState)> callback);
// POST /v1/hidden/activity: someone is using Mira, so hidden games stay unlocked a while longer.
void NoteHiddenActivityAsync(QObject* context);

}  // namespace mira_gui::api
