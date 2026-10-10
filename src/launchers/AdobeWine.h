#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

// AdobeWine (github.com/le-birnes/AdobeWine): Wine with fixes for Creative Cloud and its apps, which
// Creative Cloud's prefix runs on. Downloaded from its releases on install, never bundled.
namespace mira::launchers::adobewine {

inline constexpr std::string_view kRepo = "le-birnes/AdobeWine";

// Where its builds are unpacked, one folder each: a Wine search folder, so they're runners.
std::filesystem::path BuildsDir();

// The runner ref of its newest release, downloaded first unless it's here. Offline: the newest one here.
Result<std::string> Runtime(const std::function<void(const std::string&)>& say);

// What AdobeWine's own prefix setup does beyond winetricks: Windows 11, its Direct3D 12 shim,
// WebView2 on Wine's own dxgi, and the X11 driver without window decorations.
Result<void> Prepare(const runner::RunnerRegistry& runners, const model::Game& host,
                     const std::filesystem::path& downloads);

// Adobe's licensing window, which the apps sign in with, comes as Chromium, which doesn't draw under Wine, or
// Internet Explorer, which does: the Chromium helper is renamed away so they use the other. Creative Cloud's
// updates can put it back, so this runs on every launch.
void UseIeSignIn(const std::filesystem::path& prefix);

}  // namespace mira::launchers::adobewine
