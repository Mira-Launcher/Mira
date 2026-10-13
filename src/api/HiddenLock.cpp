#include "api/HiddenLock.h"

#include <thread>

#include "core/Password.h"

namespace mira::api {
namespace {

constexpr const char* kSecret = "hidden_password";
constexpr auto kWrongPasswordPause = std::chrono::seconds(1);

auto WrongPassword() {
  return Err("wrong_password", "that isn't the password for hidden games");
}

}  // namespace

HiddenLock::HiddenLock(store::GameStore& games) : games_(games), hash_(games.Secret(kSecret)) {}

bool HiddenLock::HasPassword() const {
  std::lock_guard lock(mutex_);
  return !hash_.empty();
}

bool HiddenLock::Locked() const {
  std::lock_guard lock(mutex_);
  return !hash_.empty() && !unlocked_;
}

Result<void> HiddenLock::SetPassword(const std::string& current, const std::string& next) {
  std::string hash;
  {
    std::lock_guard lock(mutex_);
    hash = hash_;
  }
  if (!hash.empty() && !password::Matches(current, hash)) {
    std::this_thread::sleep_for(kWrongPasswordPause);
    return WrongPassword();
  }
  std::string next_hash;
  if (!next.empty()) {
    auto hashed = password::Hash(next);
    if (!hashed) return std::unexpected(hashed.error());
    next_hash = std::move(*hashed);
  }
  if (auto saved = games_.SetSecret(kSecret, next_hash); !saved) return saved;
  std::lock_guard lock(mutex_);
  hash_ = std::move(next_hash);
  unlocked_ = true;
  active_ = std::chrono::steady_clock::now();
  return {};
}

Result<void> HiddenLock::Unlock(const std::string& password) {
  std::string hash;
  {
    std::lock_guard lock(mutex_);
    hash = hash_;
  }
  if (hash.empty()) return {};
  if (!password::Matches(password, hash)) {
    std::this_thread::sleep_for(kWrongPasswordPause);
    return WrongPassword();
  }
  std::lock_guard lock(mutex_);
  unlocked_ = true;
  active_ = std::chrono::steady_clock::now();
  return {};
}

bool HiddenLock::Lock() {
  std::lock_guard lock(mutex_);
  const bool was = !hash_.empty() && unlocked_;
  unlocked_ = false;
  return was;
}

void HiddenLock::Touch() {
  std::lock_guard lock(mutex_);
  active_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::duration HiddenLock::Idle() const {
  std::lock_guard lock(mutex_);
  return std::chrono::steady_clock::now() - active_;
}

}  // namespace mira::api
