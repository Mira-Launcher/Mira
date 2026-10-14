#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::api {

// The password on hidden games. While it's locked, mirad answers as if hidden games and hidden
// store titles weren't there (Services::Conceals). With no password set it's never locked. mirad
// starts locked, and only a slow hash of the password is kept (in mira.db).
class HiddenLock {
public:
  explicit HiddenLock(store::GameStore& games);

  bool HasPassword() const;
  bool Locked() const;

  // Sets, changes or (with `next` empty) removes the password. Once one is set, `current` has to
  // match it. Setting one leaves hidden games unlocked, since whoever set it knows it.
  Result<void> SetPassword(const std::string& current, const std::string& next);
  // A wrong password is refused after a pause, which slows guessing.
  Result<void> Unlock(const std::string& password);
  // Returns whether it was unlocked.
  bool Lock();

  // Marks someone as still using Mira, which restarts the idle count.
  void Touch();
  std::chrono::steady_clock::duration Idle() const;

private:
  store::GameStore& games_;
  mutable std::mutex mutex_;
  std::mutex set_mutex_;
  std::string hash_;
  bool unlocked_ = false;
  std::chrono::steady_clock::time_point active_ = std::chrono::steady_clock::now();
};

}  // namespace mira::api
