#pragma once

// Curated executable-name glob patterns for the detector; just data, wired
// in by Schema.cpp as defaults for detect.deny_name_patterns /
// detect.installer_name_patterns (settings.toml), which stay user-editable.

#include <array>
#include <string_view>

namespace mira::config::known_exe_patterns {

// Prerequisite installers, plus engine/launcher helper processes that ship
// inside a real game's own folder and would otherwise be auto-picked over
// the actual game.
inline constexpr std::array kDeny = {
    // prerequisite installers: redistributables commonly dropped next to a
    // game's own exe by InstallShield/Inno Setup packaging
    "unins*",
    "setup*",
    "vcredist*",
    "vc_redist*",
    "dxsetup*",
    "dxwebsetup*",
    "dotnet*",
    "ndp*",
    "directx*",
    "*redist*",
    "touchup*",
    "oalinst*",
    "physxinstaller*",
    "*physx_setup*",
    "gfwlivesetup*",
    "xnafx*",
    "*prereqsetup*",
    "*prerequisites*",
    // Unreal Engine helpers
    "*crashreport*",
    "*cefsubprocess*",
    "*crashpad*",
    // Unity helper: UnityCrashHandler*.exe getting auto-picked over the
    // real game exe (github.com/lutris/lutris/issues/6881)
    "*crashhandler*",
    // other common crash reporters (BugTrap/BugSplat, and legacy Dr. Watson)
    "*bugsplat*",
    "bssndrpt*",
    "crashsender*",
    "drwtsn32*",
    // launcher / DRM / anti-cheat helpers bundled alongside a game. These
    // ship as their own subfolder+exe in nearly every game that uses them
    // (e.g. EasyAntiCheat/EasyAntiCheat.exe, BattlEye/BEService.exe)
    "*launcher_installer*",
    "*webhelper*",
    "upc.exe",
    "*anticheat*",
    "battleye*",
    "beservice*",
    "pnkbstr*",
};

// A candidate matching one of these, combined with a size signal (see
// detect.installer_min_size_mb), is flagged as an installer rather than the
// game itself.
inline constexpr std::array kInstaller = {
    "setup*", "*setup*", "install*", "*installer*", "*_setup_*",
};

}  // namespace mira::config::known_exe_patterns
