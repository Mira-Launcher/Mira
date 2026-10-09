#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QStringList>

#include <cstdlib>
#include <filesystem>

#include "bigscreen/Session.h"
#include "client/api/Config.h"
#include "app/Appearance.h"
#include "app/DaemonSupervisor.h"
#include "app/KeyBindings.h"
#include "app/Notify.h"
#include "app/SystemNotifier.h"
#include "app/Tray.h"
#include "setup/SetupWindow.h"
#include "theme/Theme.h"
#include "widgets/ToolTip.h"
#include "window/LibraryWindow.h"

namespace {

// A bundled build (AppImage, .deb, .rpm) carries only Qt's xcb platform plugin; picking it up
// front runs through XWayland instead of failing to find wayland when started without the
// /usr/bin/mira-gui wrapper (from `mira`, autostart or Steam).
void DefaultToBundledPlatform() {
  if (std::getenv("QT_QPA_PLATFORM") != nullptr) return;
  std::error_code ec;
  const std::filesystem::path platforms =
      std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().parent_path() / "plugins" / "platforms";
  if (ec || !std::filesystem::is_directory(platforms, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(platforms, ec)) {
    if (entry.path().filename().string().starts_with("libqwayland")) return;
  }
  ::setenv("QT_QPA_PLATFORM", "xcb", 0);
}

}  // namespace

int main(int argc, char** argv) {
  DefaultToBundledPlatform();
  QApplication app(argc, argv);
  QApplication::setApplicationName("Mira");
  QApplication::setOrganizationName("mira");

  // Mira closes to tray rather than quitting, so a second launch (another
  // double-click on the AppImage, another "Mira" from the app menu) must
  // not open a second window against the same daemon; it should just no-op.
  // QLockFile detects and clears a lock left by a crashed instance on its
  // own (it checks whether the PID that holds it is still alive).
  const QString runtime_dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/mira";
  QDir().mkpath(runtime_dir);
  QLockFile single_instance_lock(runtime_dir + "/mira-gui.lock");
  // Big screen mode, for a TV and a controller (bigscreen/BigScreenWindow.h).
  const bool big_screen = app.arguments().contains("--big-screen");
  // Started at login or by `mira` with no Mira open: only mirad is wanted, so stay in the tray.
  const bool hidden = app.arguments().contains("--hidden");
  if (!single_instance_lock.tryLock(0)) {
    if (hidden) return 0;
    // Ask the instance already holding the lock to raise its own window
    // instead of just quietly doing nothing -- see the QLocalServer set up
    // below, alongside window creation.
    QLocalSocket socket;
    socket.connectToServer("mira-gui-activate");
    if (socket.waitForConnected(200)) {
      socket.write(big_screen ? "big-screen" : "activate");
      socket.waitForBytesWritten(200);
    }
    return 0;
  }
  // Matches packaging/mira.desktop, which is how the compositor and the
  // notification service work out which application this is. Only when
  // that file is actually installed: claiming an unresolvable app id
  // makes xdg-desktop-portal log a warning on every start.
  if (mira_gui::notify::system_notifier::DesktopEntryInstalled()) {
    QGuiApplication::setDesktopFileName("mira");
  }

  QIcon icon;
  for (int size : {16, 32, 48, 64, 128, 256}) {
    icon.addFile(QString(":/icons/%1x%1/apps/mira.png").arg(size), QSize(size, size));
  }
  QApplication::setWindowIcon(icon);

  // Before any window exists, so nothing is ever painted unthemed. "auto"
  // follows the desktop's light/dark preference until frontend.toml is read
  // below, once mirad answers.
  mira_gui::theme::Apply("auto");
  mira_gui::tooltip::Install();

  // docs/architecture.md's "frontend-managed" daemon path: start mirad
  // ourselves if nothing is already listening, so the AppImage works as one
  // self-contained app.
  auto* supervisor = new mira_gui::DaemonSupervisor(&app);
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Ready, &app, [big_screen, hidden] {
    // Read once and applied before the window exists, so it opens at its saved
    // size and look instead of changing once shown.
    const mira_gui::FrontendPrefsResult saved = mira_gui::api::GetFrontendPrefsBlocking();
    mira_gui::FrontendPrefs prefs = saved.ok ? saved.prefs : mira_gui::FrontendPrefs{};
    // A frontend.toml that already has a window size is an existing install, not a new one.
    const bool first_launch =
        !hidden && saved.ok && !prefs.onboarded.value_or(false) && !prefs.window_width.has_value();
    if (first_launch) {
      mira_gui::FrontendPrefs onboarded;
      onboarded.onboarded = true;
      mira_gui::api::SaveFrontendPrefsBlocking(onboarded);
    }
    mira_gui::ApplyAppearance(prefs);
    mira_gui::bigscreen::ApplyStartOnLogin(prefs.start_on_login.value_or(false));
    auto* window = new LibraryWindow(prefs);
    window->setAttribute(Qt::WA_DeleteOnClose);
    // A no-op on a desktop with no tray (Tray.cpp): window->close() then
    // means exactly what it always did.
    mira_gui::tray::Attach(window);
    mira_gui::bigscreen::UpdateSteamShortcut(window);
    if (first_launch) {
      // Set up Mira opens instead of the main window, which shows once it closes.
      QObject::connect(window->OpenSetup(prefs), &mira_gui::SetupWindow::Finished, window,
                       [window](bool big_screen) {
                         if (big_screen) return window->OpenBigScreen();
                         window->show();
                       });
    } else if (hidden) {
      // With no tray to come back from, the dock or taskbar.
      if (!mira_gui::tray::Available()) window->showMinimized();
    } else if (big_screen || prefs.big_screen_at_start.value_or(false)) {
      window->OpenBigScreen();
    } else {
      window->show();
    }

    // Raises this window when a second launch pings "activate". removeServer
    // clears a stale socket left by a crashed instance.
    QLocalServer::removeServer("mira-gui-activate");
    auto* activation_server = new QLocalServer(window);
    activation_server->listen("mira-gui-activate");
    QObject::connect(activation_server, &QLocalServer::newConnection, window, [activation_server, window] {
      QLocalSocket* client = activation_server->nextPendingConnection();
      QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
      QObject::connect(client, &QLocalSocket::readyRead, window, [client, window] {
        if (client->readAll() == "big-screen") {
          window->OpenBigScreen();
        } else {
          if (window->isMinimized()) window->showNormal();
          window->show();
          window->raise();
          window->activateWindow();
        }
        client->disconnectFromServer();
      });
    });
  });
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Failed, &app, [](QString error) {
    mira_gui::notify::FailedWithHint(
        nullptr, "Could not start mirad.", error,
        "Mira looks for \"mirad\" next to its own binary, then on PATH. Build it "
        "(cmake --build build --target mirad) or reinstall Mira.");
    QApplication::quit();
  });
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Outdated, &app, [](int api) {
    mira_gui::notify::FailedWithHint(
        nullptr, "The running mirad doesn't match this version of Mira.",
        QString("It speaks API %1; this app needs API %2.").arg(api).arg(mira_gui::kExpectedApiVersion),
        "Quit Mira from the tray, or stop the mirad process, and start Mira again.");
    QApplication::quit();
  });
  supervisor->EnsureRunning();

  return QApplication::exec();
}
