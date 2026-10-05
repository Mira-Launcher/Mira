// mirad: the Mira backend daemon.
//
// Owns settings.toml and games.toml, serves the REST API described in
// docs/api.md over a Unix domain socket, and watches every enabled library
// root so a dropped-in game folder is picked up automatically (see
// library/Watcher.h). This binary does not daemonize itself (no
// double-fork): run it under `systemctl --user`, or let the frontend spawn
// and supervise it. Both are first-class per the plan in
// docs/architecture.md, and neither needs mirad to background itself.

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <format>
#include <thread>

#include "api/EventBus.h"
#include "api/Server.h"
#include "api/Services.h"
#include "config/Config.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "library/Scanner.h"
#include "library/Watcher.h"
#include "metadata/MetadataFetcher.h"
#include "runner/RefMigration.h"
#include "store/GameStore.h"

namespace {

void PrintUsage() {
  std::puts(
      "usage: mirad [--socket PATH] [--version] [--help]\n"
      "\n"
      "Runs in the foreground. Stop with SIGINT/SIGTERM (Ctrl-C, or the\n"
      "service manager's normal stop).");
}

// Blocks the calling thread until SIGINT or SIGTERM arrives, with no polling,
// just a single blocking syscall, which is what lets the main thread cost
// nothing while the server thread does the real work.
void WaitForShutdownSignal() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  int signal = 0;
  sigwait(&set, &signal);
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path socket_override;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    }
    if (arg == "--version") {
      std::puts("mirad " MIRA_VERSION);
      return 0;
    }
    if (arg == "--socket" && i + 1 < argc) {
      socket_override = argv[++i];
      continue;
    }
    std::fprintf(stderr, "mirad: unrecognised argument \"%.*s\"\n", static_cast<int>(arg.size()),
                arg.data());
    PrintUsage();
    return 2;
  }

  mira::config::Config config(mira::paths::SettingsFile());
  config.Load();
  if (const std::string level = config.GetString("log.level"); level == "debug") {
    mira::log::SetLevel(mira::log::Level::Debug);
  } else if (level == "warn") {
    mira::log::SetLevel(mira::log::Level::Warn);
  } else if (level == "error") {
    mira::log::SetLevel(mira::log::Level::Error);
  }

  mira::store::GameStore games(mira::paths::GamesFile());
  games.Load();

  mira::api::EventBus events;
  mira::api::Services services(config, games, events);
  mira::api::Server server(services);

  // Before anything else: close out any session a previous mirad (crashed,
  // killed, or just restarted) left behind; see
  // proc::ProcessSupervisor::Reconcile and docs/architecture.md. Must run
  // before Serve() so a re-adopted still-running game is already tracked by
  // the time the very first client request arrives.
  services.ReconcileSessions();
  mira::runner::MigrateInPlaceRefs(config, games);
  // Left over if the last GUI never got to clear them (killed, or crashed).
  mira::metadata::ClearCandidateThumbs(config);

  const std::filesystem::path socket_path =
      socket_override.empty() ? mira::paths::Expand(config.GetString("socket_path")) : socket_override;

  // Block SIGINT/SIGTERM on every thread before any is created, so only the
  // dedicated WaitForShutdownSignal() call below ever receives them.
  sigset_t block_set;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGINT);
  sigaddset(&block_set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &block_set, nullptr);

  // A game folder that already existed before mirad started produces no
  // inotify event: inotify only reports changes from here on, not existing
  // state, so a full reconcile has to run once. Also catches a folder that
  // appeared while the daemon was down. It runs beside the server, not before
  // it: provisioning a new Windows game takes seconds each, and clients must
  // not wait on that to connect. Scans and the watcher serialize on
  // GameStore::LockFolders.
  mira::library::CreateMissingRoots(config);
  std::thread startup_scan_thread([&] {
    mira::library::Scanner startup_scan(config, games, events);
    startup_scan.UseMetadataQueue(services.fetches);
    startup_scan.UseInstallLane(services.installs);
    const mira::library::ScanSummary summary = startup_scan.ScanAll();
    mira::log::Info("startup scan: added {}, missing {}, restored {}", summary.added,
                    summary.missing, summary.restored);
    services.QueueMetadata(summary.added_games);
  });

  mira::library::Watcher watcher(config, games, events);
  watcher.UseMetadataQueue(services.fetches);
  watcher.UseInstallLane(services.installs);
  services.on_roots_changed = [&watcher] { watcher.ReloadRoots(); };
  std::thread watcher_thread([&] { watcher.Run(); });

  std::atomic<bool> serve_failed{false};
  std::thread server_thread([&] {
    if (auto result = server.Serve(socket_path); !result) {
      mira::log::Error("server exited: {}", result.error().message);
      serve_failed = true;
      kill(getpid(), SIGTERM);  // wakes WaitForShutdownSignal so the daemon doesn't linger with no socket
    }
  });

  WaitForShutdownSignal();
  mira::log::Info("shutting down");
  server.Stop();
  watcher.Stop();
  server_thread.join();
  watcher_thread.join();
  startup_scan_thread.join();
  mira::metadata::ClearCandidateThumbs(config);
  return serve_failed ? 1 : 0;
}
