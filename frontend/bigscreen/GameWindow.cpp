#include "GameWindow.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#ifdef MIRA_HAVE_XCB
#include <xcb/xcb.h>
#endif

namespace mira_gui::bigscreen {

namespace {

// Whether `pid` is in the game cgroup of the mira-run running `game_id`. mira-run sits in the scope
// above its "game" group, titled "mira-run: <game id>" (src/wrapper/main.cpp).
[[maybe_unused]] bool RunsGame(std::uint32_t pid, const std::string& game_id) {
  std::ifstream cgroup_file("/proc/" + std::to_string(pid) + "/cgroup");
  std::string path;
  for (std::string line; std::getline(cgroup_file, line);) {
    if (line.starts_with("0::")) path = line.substr(3);
  }
  const std::filesystem::path group = "/sys/fs/cgroup" + path;
  if (path.empty() || group.filename() != "game") return false;
  std::ifstream procs(group.parent_path() / "cgroup.procs");
  for (std::string wrapper; std::getline(procs, wrapper);) {
    std::ifstream cmdline("/proc/" + wrapper + "/cmdline");
    std::string title;
    std::getline(cmdline, title, '\0');
    if (title == "mira-run: " + game_id) return true;
  }
  return false;
}

}  // namespace

#ifdef MIRA_HAVE_XCB

namespace {

struct FreeDeleter {
  void operator()(void* p) const { std::free(p); }
};
template <typename T>
using Reply = std::unique_ptr<T, FreeDeleter>;

// Its own connection, so it works whichever platform Qt runs on; null with no X server.
xcb_connection_t* Connection() {
  static xcb_connection_t* connection = nullptr;
  if (connection != nullptr && xcb_connection_has_error(connection) != 0) {
    xcb_disconnect(connection);
    connection = nullptr;
  }
  if (connection == nullptr) {
    connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection) != 0) {
      xcb_disconnect(connection);
      connection = nullptr;
    }
  }
  return connection;
}

xcb_atom_t Atom(xcb_connection_t* c, const char* name) {
  const Reply<xcb_intern_atom_reply_t> reply(
      xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, std::uint16_t(std::strlen(name)), name), nullptr));
  return reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
}

Reply<xcb_get_property_reply_t> Property(xcb_connection_t* c, xcb_window_t window, xcb_atom_t property,
                                         xcb_atom_t type, std::uint32_t length) {
  xcb_generic_error_t* error = nullptr;  // a window closed meanwhile
  Reply<xcb_get_property_reply_t> reply(
      xcb_get_property_reply(c, xcb_get_property(c, 0, window, property, type, 0, length), &error));
  std::free(error);
  return reply;
}

// Whether `window` holds `atom` in its `property` atom list.
bool HasAtom(xcb_connection_t* c, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom) {
  const auto list = Property(c, window, property, XCB_ATOM_ATOM, 64);
  if (!list) return false;
  const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(list.get()));
  return std::find(atoms, atoms + xcb_get_property_value_length(list.get()) / 4, atom) !=
         atoms + xcb_get_property_value_length(list.get()) / 4;
}

// A window the game plays in, not its dialog or splash screen.
bool IsMainWindow(xcb_connection_t* c, xcb_window_t window) {
  const auto transient = Property(c, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 1);
  if (transient && xcb_get_property_value_length(transient.get()) > 0) return false;
  const auto type = Property(c, window, Atom(c, "_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 1);
  return !type || xcb_get_property_value_length(type.get()) == 0 ||
         *static_cast<const xcb_atom_t*>(xcb_get_property_value(type.get())) == Atom(c, "_NET_WM_WINDOW_TYPE_NORMAL");
}

void SendToRoot(xcb_connection_t* c, xcb_window_t root, xcb_window_t window, xcb_atom_t type,
                std::initializer_list<std::uint32_t> data) {
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = window;
  event.type = type;
  std::copy(data.begin(), data.end(), event.data.data32);
  xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&event));
}

}  // namespace

bool FocusGameWindow(const std::string& game_id, bool fullscreen) {
  xcb_connection_t* c = Connection();
  if (c == nullptr) return false;
  const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  const xcb_atom_t client_list = Atom(c, "_NET_CLIENT_LIST");
  const xcb_atom_t wm_pid = Atom(c, "_NET_WM_PID");
  const auto list = Property(c, root, client_list, XCB_ATOM_WINDOW, 4096);
  if (!list) return false;
  const auto* windows = static_cast<const xcb_window_t*>(xcb_get_property_value(list.get()));
  // Newest last, in mapping order.
  for (int i = xcb_get_property_value_length(list.get()) / 4 - 1; i >= 0; --i) {
    const auto pid = Property(c, windows[i], wm_pid, XCB_ATOM_CARDINAL, 1);
    if (!pid || xcb_get_property_value_length(pid.get()) != 4) continue;
    if (!RunsGame(*static_cast<const std::uint32_t*>(xcb_get_property_value(pid.get())), game_id)) continue;
    // Source 2 (a pager): window managers honor it without focus-stealing checks.
    SendToRoot(c, root, windows[i], Atom(c, "_NET_ACTIVE_WINDOW"), {2, XCB_CURRENT_TIME});
    const xcb_atom_t state = Atom(c, "_NET_WM_STATE");
    const xcb_atom_t full = Atom(c, "_NET_WM_STATE_FULLSCREEN");
    if (fullscreen && IsMainWindow(c, windows[i]) && !HasAtom(c, windows[i], state, full)) {
      SendToRoot(c, root, windows[i], state, {1, full, 0, 2});  // 1: add
    }
    xcb_flush(c);
    return true;
  }
  return false;
}

#else

bool FocusGameWindow(const std::string&, bool) { return false; }

#endif

}  // namespace mira_gui::bigscreen
