#include "runner/GameMode.h"

#include <string>
#include <vector>

#include "core/Command.h"
#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::gamemode {
namespace {

constexpr char kBusName[] = "com.feralinteractive.GameMode";
constexpr char kObjectPath[] = "/com/feralinteractive/GameMode";

// One D-Bus method call, translated into whichever session-bus CLI is
// available -- doesn't assume Qt/GTK/a particular toolkit, just a session
// bus. `gdbus` needs glib2 (not guaranteed on a Qt-only system); `dbus-send`
// ships with dbus itself. Neither is linked, matching the shell-out
// convention used for curl/sqlite3/flatpak elsewhere; neither present just
// means GameMode integration no-ops cleanly.
struct DBusCall {
  std::string dest;
  std::string object_path;
  std::string method;  // "interface.Method"
  std::string
      gdbus_arg;  // GVariant text-format literal, e.g. "42" or the bare bus name; empty for none
  std::string
      dbus_send_arg;  // dbus-send's typed syntax, e.g. "int32:42" or "string:..."; empty for none
};

Result<runner::ExecResult> Run(const DBusCall& call) {
  if (const auto gdbus = runner::FindOnPath("gdbus")) {
    Command command;
    command.argv = {*gdbus,          "call",           "--session", "--dest",   call.dest,
                    "--object-path", call.object_path, "--method",  call.method};
    if (!call.gdbus_arg.empty()) command.argv.push_back(call.gdbus_arg);
    return runner::RunAndWait(command);
  }
  if (const auto dbus_send = runner::FindOnPath("dbus-send")) {
    Command command;
    command.argv = {*dbus_send,       "--session", "--print-reply", "--dest=" + call.dest,
                    call.object_path, call.method};
    if (!call.dbus_send_arg.empty()) command.argv.push_back(call.dbus_send_arg);
    return runner::RunAndWait(command);
  }
  return Err("dbus_tool_missing", "neither gdbus nor dbus-send is installed");
}

}  // namespace

bool IsInstalled() {
  return runner::FindOnPath("gamemoded") || runner::FindOnPath("gamemoderun");
}

bool IsDaemonRunning() {
  // The desktop-bus-standard NameHasOwner call, not anything GameMode-
  // specific -- works the same regardless of which bus daemon owns the
  // session bus. gdbus reports "(true,)"/"(false,)"; dbus-send reports
  // "boolean true"/"boolean false" -- checking for "true" alone covers both.
  const auto result = Run({.dest = "org.freedesktop.DBus",
                           .object_path = "/org/freedesktop/DBus",
                           .method = "org.freedesktop.DBus.NameHasOwner",
                           .gdbus_arg = kBusName,
                           .dbus_send_arg = std::string("string:") + kBusName});
  return result && result->exit_code == 0 && result->output.find("true") != std::string::npos;
}

void RegisterGame(pid_t pid) {
  const auto result = Run({.dest = kBusName,
                           .object_path = kObjectPath,
                           .method = std::string(kBusName) + ".RegisterGame",
                           .gdbus_arg = std::to_string(pid),
                           .dbus_send_arg = "int32:" + std::to_string(pid)});
  if (!result || result->exit_code != 0) {
    // Never a launch failure -- GameMode not being reachable is exactly as
    // harmless as the game requesting it directly and getting no answer.
    log::Warn("GameMode registration for pid {} failed (daemon not running?): {}", pid,
              !result ? result.error().message : result->output);
  }
}

void UnregisterGame(pid_t pid) {
  const auto result = Run({.dest = kBusName,
                           .object_path = kObjectPath,
                           .method = std::string(kBusName) + ".UnregisterGame",
                           .gdbus_arg = std::to_string(pid),
                           .dbus_send_arg = "int32:" + std::to_string(pid)});
  if (!result || result->exit_code != 0) {
    log::Warn("GameMode unregistration for pid {} failed: {}", pid,
              !result ? result.error().message : result->output);
  }
}

}  // namespace mira::gamemode
