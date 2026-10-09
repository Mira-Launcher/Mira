#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QStringList>

#include "client/api/Config.h"
#include "dialogs/FirstRunWizard.h"
#include "app/Appearance.h"
#include "app/DaemonSupervisor.h"
#include "app/KeyBindings.h"
#include "app/Notify.h"
#include "app/SystemNotifier.h"
#include "app/Tray.h"
#include "theme/Theme.h"
#include "widgets/ToolTip.h"
#include "window/LibraryWindow.h"

int main(int argc, char** argv) {
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
  if (!single_instance_lock.tryLock(0)) {
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
  // self-contained app with no systemd unit required.
  auto* supervisor = new mira_gui::DaemonSupervisor(&app);
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Ready, &app, [big_screen] {
    // Read once and applied before the window exists, so it opens at its saved
    // size and look instead of changing once shown.
    const mira_gui::FrontendPrefsResult saved = mira_gui::api::GetFrontendPrefsBlocking();
    mira_gui::FrontendPrefs prefs = saved.ok ? saved.prefs : mira_gui::FrontendPrefs{};
    if (saved.ok && mira_gui::FirstRunWizard::Needed(prefs)) {
      mira_gui::FirstRunWizard wizard(prefs);
      wizard.exec();
      // What it chose decides how the window opens.
      const mira_gui::FrontendPrefsResult after = mira_gui::api::GetFrontendPrefsBlocking();
      if (after.ok) prefs = after.prefs;
    }
    mira_gui::ApplyAppearance(prefs);
    auto* window = new LibraryWindow(prefs);
    window->setAttribute(Qt::WA_DeleteOnClose);
    // A no-op on a desktop with no tray (Tray.cpp): window->close() then
    // means exactly what it always did.
    mira_gui::tray::Attach(window);
    if (big_screen || prefs.big_screen_at_start.value_or(false)) {
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
        "(cmake --build build --target mirad) or install the package that provides it, or "
        "start it yourself first: run \"mirad\" in a terminal, or run "
        "systemctl --user enable --now mirad.service to start it with your session.");
    QApplication::quit();
  });
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Outdated, &app, [](int api) {
    mira_gui::notify::FailedWithHint(
        nullptr, "The running mirad doesn't match this version of Mira.",
        QString("It speaks API %1; this app needs API %2.").arg(api).arg(mira_gui::kExpectedApiVersion),
        "Restart it so it picks up the update: systemctl --user restart mirad.service, or stop the mirad "
        "process and start Mira again.");
    QApplication::quit();
  });
  supervisor->EnsureRunning();

  return QApplication::exec();
}
