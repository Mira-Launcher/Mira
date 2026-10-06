// mira: command-line client for mirad.
//
// A pure REST client over the same Unix socket the frontend uses: no direct
// access to settings.toml/games.toml, no special privilege.

#include <httplib.h>
#include <json.hpp>
#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "config/Schema.h"
#include "core/TomlJson.h"
#include "core/Paths.h"
#include "setup/Setup.h"

namespace {
using nlohmann::json;

// Read-only: Config::Load would write defaults or quarantine a bad file
// under the running daemon.
std::filesystem::path ResolveSocketPath() {
  static const std::filesystem::path path = [] {
    const auto pointer = mira::config::Schema::Pointer("socket_path");
    json settings = mira::config::Schema::Instance().Defaults();
    if (toml::parse_result parsed = toml::parse_file(mira::paths::SettingsFile().string()); parsed) {
      const json saved = mira::tomljson::ToJson(parsed.table());
      if (saved.contains(pointer) && saved[pointer].is_string()) settings[pointer] = saved[pointer];
    }
    return mira::paths::Expand(settings[pointer].get<std::string>());
  }();
  return path;
}

httplib::Client Connect() {
  httplib::Client client(ResolveSocketPath().string(), 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));
  return client;
}

// The command that does an error's `fix` (docs/api.md), or "" when there isn't one.
std::string FixCommand(const json& fix) {
  const std::string kind = fix.value("kind", std::string());
  const std::string target = fix.value("target", std::string());
  const std::string step = fix.value("step", std::string());
  if (kind == "setting") return std::format("mira config set {} <value>", target);
  if (kind == "runners") return target.empty() ? "mira runners catalog" : "mira runners tools install " + target;
  if (kind == "source") return step == "install" ? "mira launcher install " + target : std::format("mira store {} {}", target, step);
  if (kind == "game" && step == "exe") return std::format("mira set {} --exe <path>", target);
  if (kind == "game" && step == "data_dir") return std::format("mira set {} --data-dir <path>", target);
  return "";
}

void PrintError(const httplib::Result& res) {
  if (res) {
    const json body = json::parse(res->body, nullptr, false);
    const json error = body.is_object() ? body.value("error", json::object()) : json::object();
    const std::string message = error.value("message", res->body);
    std::fprintf(stderr, "mira: %s (HTTP %d)\n", message.c_str(), res->status);
    const std::string hint = error.value("hint", std::string());
    const std::string command = error.contains("fix") ? FixCommand(error["fix"]) : std::string();
    if (!hint.empty()) std::fprintf(stderr, "      %s\n", hint.c_str());
    if (!command.empty()) std::fprintf(stderr, "      Try: %s\n", command.c_str());
  } else {
    std::fprintf(stderr,
                 "mira: cannot reach mirad at %s (%s). Is it running? "
                 "Try `systemctl --user status mirad` or just running `mirad`.\n",
                 ResolveSocketPath().string().c_str(), httplib::to_string(res.error()).c_str());
  }
}

bool Ok(const httplib::Result& res) { return res && res->status >= 200 && res->status < 300; }

// A long request's reply is 202 {job}: waits for that job (docs/api.md, Jobs)
// and fills `result` with what it returned. Prints why and returns false if
// the request or the job failed.
bool AwaitJob(httplib::Client& client, const httplib::Result& res, json& result) {
  if (!Ok(res)) {
    PrintError(res);
    return false;
  }
  const json started = json::parse(res->body, nullptr, false);
  const std::string id = started.is_object() ? started.value("job", std::string()) : std::string();
  if (id.empty()) {
    result = started;
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
  while (std::chrono::steady_clock::now() < deadline) {
    auto job = client.Get("/v1/jobs/" + id);
    if (!Ok(job)) {
      PrintError(job);
      return false;
    }
    const json state = json::parse(job->body, nullptr, false);
    const std::string done = state.value("state", std::string());
    if (done == "finished") {
      result = state.value("result", json::object());
      return true;
    }
    if (done == "failed") {
      const json error = state.value("error", json::object());
      std::fprintf(stderr, "mira: %s\n", error.value("message", std::string("the job failed")).c_str());
      const std::string hint = error.value("hint", std::string());
      const std::string command = error.contains("fix") ? FixCommand(error["fix"]) : std::string();
      if (!hint.empty()) std::fprintf(stderr, "      %s\n", hint.c_str());
      if (!command.empty()) std::fprintf(stderr, "      Try: %s\n", command.c_str());
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::fprintf(stderr, "mira: gave up waiting for job %s; it may still be running\n", id.c_str());
  return false;
}

int CmdStatus() {
  auto client = Connect();
  auto res = client.Get("/v1/health");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("mirad is running");
  const json health = json::parse(res->body, nullptr, false);
  if (!health.is_object() || health.value("api", 0) != mira::kApiVersion) {
    std::fprintf(stderr, "mira: mirad's API doesn't match this mira; restart it (systemctl --user restart mirad)\n");
    return 1;
  }
  return 0;
}

int CmdScan() {
  auto client = Connect();
  json summary;
  if (!AwaitJob(client, client.Post("/v1/library/scan"), summary)) return 1;
  std::printf("added: %lld  missing: %lld  restored: %lld\n",
             summary.value("added", 0LL), summary.value("missing", 0LL),
             summary.value("restored", 0LL));
  return 0;
}

int CmdLibraryRelocate() {
  auto client = Connect();
  json summary;
  if (!AwaitJob(client, client.Post("/v1/library/relocate"), summary)) return 1;
  std::printf("moved: %lld  failed: %lld\n", summary.value("moved", 0LL), summary.value("failed", 0LL));
  return 0;
}

int CmdRunnersList() {
  auto client = Connect();
  auto res = client.Get("/v1/runners");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json builds = json::parse(res->body);
  if (builds.empty()) {
    std::puts("(no runner builds found, check runner_search_paths / wine_search_paths)");
    return 0;
  }
  for (const json& build : builds) {
    std::printf("%-40s %s\n", build.value("reference", "").c_str(), build.value("path", "").c_str());
  }
  return 0;
}

// Prints `path`'s JSON array, one line per entry via `line`.
int PrintList(const std::string& path, const char* empty, const std::function<void(const json&)>& line) {
  auto client = Connect();
  auto res = client.Get(path);
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  const json entries = json::parse(res->body);
  if (entries.empty()) std::puts(empty);
  for (const json& entry : entries) line(entry);
  return 0;
}

int CmdRunnersSources(int argc, char** argv) {
  const std::string kind = argc > 0 ? argv[0] : "";
  return PrintList("/v1/runners/sources?kind=" + kind, "(no sources)", [](const json& s) {
    std::printf("%-20s %-8s %s\n", s.value("id", "").c_str(), s.value("kind", "").c_str(), s.value("label", "").c_str());
  });
}

int CmdRunnersUpdates() {
  return PrintList("/v1/runners/updates", "(everything is up to date)", [](const json& u) {
    std::printf("%-40s -> %s\n", u.value("reference", "").c_str(), u.value("name", "").c_str());
  });
}

int CmdRunnersUpdate(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira runners update <kind:name>\n"
                 "  installs the newest release from that build's source and moves its games onto it.\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post("/v1/runners/update", json{{"reference", argv[0]}}.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::printf("updating to %s: watch `mira watch` for runners.download.finished/failed\n",
             json::parse(res->body).value("name", "").c_str());
  return 0;
}

int CmdRunnersTools(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[0]) == "install") {
    auto client = Connect();
    auto res = client.Post(std::format("/v1/runners/tools/{}/setup", argv[1]));
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::printf("installing %s: watch `mira watch` for %s.setup.finished/failed\n", argv[1], argv[1]);
    return 0;
  }
  return PrintList("/v1/runners/tools", "", [](const json& t) {
    std::printf("%-12s %-14s %s\n", t.value("id", "").c_str(),
               t.value("installed", false) ? "installed" : "not installed", t.value("path", "").c_str());
  });
}

int CmdRunnersCatalog(int argc, char** argv) {
  std::string kind = "proton", source;
  for (int i = 0; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--kind" && i + 1 < argc) kind = argv[++i];
    else if (std::string_view(argv[i]) == "--source" && i + 1 < argc) source = argv[++i];
  }
  auto client = Connect();
  auto res = client.Get(std::format("/v1/runners/catalog?kind={}&source={}", kind, source));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json releases = json::parse(res->body);
  if (releases.empty()) {
    std::puts("(no releases found, check runner_sources.*.repo / .asset_pattern)");
    return 0;
  }
  for (const json& r : releases) {
    std::printf("%-24s %-40s %8.1f MB%s%s\n", r.value("tag", "").c_str(), r.value("asset_name", "").c_str(),
               r.value("size_bytes", 0LL) / 1024.0 / 1024.0,
               r.value("has_checksum", false) ? "" : "  (no checksum)",
               r.value("installed", false) ? "  (installed)" : "");
  }
  return 0;
}

int CmdRunnersDownload(int argc, char** argv) {
  std::string kind, tag, source;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--kind" && i + 1 < argc) kind = argv[++i];
    else if (arg == "--tag" && i + 1 < argc) tag = argv[++i];
    else if (arg == "--source" && i + 1 < argc) source = argv[++i];
  }
  if (kind.empty() || tag.empty()) {
    std::fprintf(stderr, "usage: mira runners download --kind proton|wine --tag TAG [--source ID]\n");
    return 2;
  }
  auto client = Connect();
  const json body = {{"kind", kind}, {"tag", tag}, {"source", source}};
  auto res = client.Post("/v1/runners/download", body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::printf("downloading %s %s: watch `mira watch` for runners.download.finished/failed\n",
             kind.c_str(), tag.c_str());
  return 0;
}

int CmdRunnersSchema(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira runners schema <kind>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Get(std::format("/v1/runners/{}/schema", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdRunnersRemove(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira runners remove <kind:name>\n"
                 "  uninstalls a build fetched via `runners download` (e.g. proton:GE-Proton11-7).\n"
                 "  Refuses anything not really inside runner_search_paths/wine_search_paths --\n"
                 "  the system wine, or a hand-edited path, can't be removed this way.\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Delete(std::format("/v1/runners/{}", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("removed");
  return 0;
}

int CmdRunners(int argc, char** argv) {
  if (argc == 0) return CmdRunnersList();
  const std::string_view sub = argv[0];
  if (sub == "catalog") return CmdRunnersCatalog(argc - 1, argv + 1);
  if (sub == "download") return CmdRunnersDownload(argc - 1, argv + 1);
  if (sub == "schema") return CmdRunnersSchema(argc - 1, argv + 1);
  if (sub == "remove") return CmdRunnersRemove(argc - 1, argv + 1);
  if (sub == "sources") return CmdRunnersSources(argc - 1, argv + 1);
  if (sub == "updates") return CmdRunnersUpdates();
  if (sub == "update") return CmdRunnersUpdate(argc - 1, argv + 1);
  if (sub == "tools") return CmdRunnersTools(argc - 1, argv + 1);
  std::fprintf(stderr,
              "usage: mira runners [sources [KIND] | catalog [--kind K] [--source ID] |\n"
              "                     download --kind K --tag TAG [--source ID] | updates | update <kind:name> |\n"
              "                     tools [install umu|winetricks] | schema <kind> | remove <kind:name>]\n");
  return 2;
}

int CmdLaunch(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira launch <id>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/launch", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running");
  return 0;
}

int CmdStop(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira stop <id>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/stop", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("stopping");
  return 0;
}

int CmdRun(int argc, char** argv) {
  std::string id, exe_path, args;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--exe" && i + 1 < argc) exe_path = argv[++i];
    else if (arg == "--args" && i + 1 < argc) args = argv[++i];
    else if (id.empty()) id = arg;
  }
  if (id.empty() || exe_path.empty()) {
    std::fprintf(stderr,
                 "usage: mira run <id> --exe PATH [--args ARGS]\n"
                 "  runs an arbitrary exe inside this game's own prefix (tracked like a normal\n"
                 "  launch). It provisions one first if it doesn't have one yet, which is how a\n"
                 "  needs_install game's installer actually gets run.\n");
    return 2;
  }
  auto client = Connect();
  json body = {{"exe_path", exe_path}};
  if (!args.empty()) body["args"] = args;
  auto res = client.Post(std::format("/v1/games/{}/run", id), body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running");
  return 0;
}

int CmdAdd(int argc, char** argv) {
  std::string install_path, exe_path, name, platform;
  bool is_installer = false;
  std::vector<std::string> positional;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--name" && i + 1 < argc) name = argv[++i];
    else if (arg == "--platform" && i + 1 < argc) platform = argv[++i];
    else if (arg == "--installer") is_installer = true;
    else positional.push_back(std::string(arg));
  }
  if (positional.size() < 2) {
    std::fprintf(stderr,
                 "usage: mira add <install_path> <exe_path> [--name N] "
                 "[--platform windows|native] [--installer]\n"
                 "  creates a game record for a path outside anywhere Mira already scans --\n"
                 "  point it at an installer with --installer, run it with `mira run`, then\n"
                 "  `mira finish-install` once it's actually installed.\n");
    return 2;
  }
  json body = {{"install_path", positional[0]}, {"exe_path", positional[1]}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;
  if (is_installer) body["is_installer"] = true;

  auto client = Connect();
  auto res = client.Post("/v1/games/manual", body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json game = json::parse(res->body);
  std::printf("%s: %s\n", game.value("id", "").c_str(), game.value("status", "").c_str());
  return 0;
}

int CmdInstall(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira install <id> [--interactive] [--installer PATH]\n"
                 "       mira install <id> --info [--installer PATH]\n"
                 "       mira install <id> --progress\n");
    return 2;
  }
  const std::string id = argv[0];
  bool interactive = false;
  bool info = false;
  bool progress = false;
  std::string installer;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--interactive") {
      interactive = true;
    } else if (arg == "--info") {
      info = true;
    } else if (arg == "--progress") {
      progress = true;
    } else if (arg == "--installer" && i + 1 < argc) {
      installer = std::filesystem::absolute(argv[++i]).string();
    } else {
      std::fprintf(stderr, "mira: unknown install option \"%s\"\n", argv[i]);
      return 2;
    }
  }

  auto client = Connect();
  httplib::Result res;
  if (info) {
    httplib::Params params;
    if (!installer.empty()) params.emplace("path", installer);
    res = client.Get(std::format("/v1/games/{}/installer", id), params, httplib::Headers{});
  } else if (progress) {
    res = client.Get(std::format("/v1/games/{}/install/progress", id));
  } else {
    json body = {{"interactive", interactive}};
    if (!installer.empty()) body["installer"] = installer;
    res = client.Post(std::format("/v1/games/{}/install", id), body.dump(), "application/json");
  }
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  if (info || progress) {
    std::puts(json::parse(res->body).dump(2).c_str());
  } else {
    std::printf("installing: `mira install %s --progress` to check on it\n", id.c_str());
  }
  return 0;
}

int CmdFinishInstall(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira finish-install <id>\n"
                 "  after running an installer with `mira run` and pointing --exe at whatever\n"
                 "  it actually installed via `mira set <id> --exe ...`, this marks the game\n"
                 "  ready to launch normally.\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/finish-install", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("ready");
  return 0;
}

int CmdRelocate(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira relocate <id>\n"
                 "  moves this game's files into Mira's own canonical layout under\n"
                 "  library_roots/prefix_root (see prefix_naming) -- the escape hatch after a\n"
                 "  Lutris import, or after changing where prefix_root points.\n");
    return 2;
  }
  auto client = Connect();
  json game;
  if (!AwaitJob(client, client.Post(std::format("/v1/games/{}/relocate", argv[0])), game)) return 1;
  std::printf("%s: install_path=%s data_dir=%s\n", game.value("id", "").c_str(),
             game.value("install_path", "").c_str(), game.value("data_dir", "").c_str());
  return 0;
}

int CmdRemove(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira remove <id> [--delete-files] [--delete-prefix] [--delete-metadata] [--purge]\n"
                 "  forgets the game; nothing on disk is touched unless you ask for it\n"
                 "  explicitly, and only paths that are really inside a configured root.\n"
                 "  --delete-files     removes the game's own install folder\n"
                 "  --delete-prefix    removes its Wine/Proton prefix; use --delete-files\n"
                 "                     alone to remove the game but leave the prefix in place\n"
                 "  --delete-metadata  removes cached cover art / store info\n"
                 "  --purge            shorthand for all three of the above\n");
    return 2;
  }
  const std::string id = argv[0];
  bool delete_files = false, delete_prefix = false, delete_metadata = false, purge = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--delete-files") delete_files = true;
    else if (arg == "--delete-prefix") delete_prefix = true;
    else if (arg == "--delete-metadata") delete_metadata = true;
    else if (arg == "--purge") purge = true;
  }
  auto client = Connect();
  std::string path = std::format("/v1/games/{}", id);
  if (delete_files || delete_prefix || delete_metadata || purge) {
    path += "?";
    if (delete_files) path += "delete_files=true&";
    if (delete_prefix) path += "delete_prefix=true&";
    if (delete_metadata) path += "delete_metadata=true&";
    if (purge) path += "purge=true&";
    path.pop_back();  // trailing '&' or '?'
  }
  auto res = client.Delete(path);
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("removed");
  return 0;
}

int CmdSteamScan() {
  auto client = Connect();
  json summary;
  if (!AwaitJob(client, client.Post("/v1/steam/scan"), summary)) return 1;
  std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
  return 0;
}

int CmdSteamStatus(const char* status) {
  auto client = Connect();
  auto res = client.Post("/v1/steam/status", json{{"status", status}}.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  return 0;
}

int CmdSteam(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "scan") return CmdSteamScan();
  if (argc > 1 && std::string_view(argv[0]) == "status") return CmdSteamStatus(argv[1]);
  std::fprintf(stderr, "usage: mira steam scan\n       mira steam status online|invisible\n");
  return 2;
}

int CmdLutrisImport() {
  auto client = Connect();
  json summary;
  if (!AwaitJob(client, client.Post("/v1/lutris/import"), summary)) return 1;
  std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
  if (const long long other = summary.value("other_runner", 0LL); other > 0) {
    std::printf("%lld left to another runner (Steam, Flatpak, ...). They stay in Lutris, and a Steam "
                "game is picked up by `mira steam scan`\n",
                other);
  }
  if (const long long incomplete = summary.value("incomplete", 0LL); incomplete > 0) {
    std::printf("%lld not imported: their Lutris config has no prefix or no absolute exe path\n",
                incomplete);
  }
  return 0;
}

int CmdLutris(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "import") return CmdLutrisImport();
  std::fprintf(stderr, "usage: mira lutris import\n");
  return 2;
}

int CmdLauncher(int argc, char** argv) {
  const std::string_view sub = argc > 0 ? argv[0] : "";
  auto client = Connect();
  if (sub == "list") {
    auto res = client.Get("/v1/launchers");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    for (const json& launcher : json::parse(res->body)) {
      const std::string state = launcher.value("installed", false) ? "installed"
                                : launcher.value("install_state", "") == "running" ? "installing"
                                                                                  : "not installed";
      std::printf("%-10s %-16s %s\n", launcher.value("id", "").c_str(), launcher.value("name", "").c_str(),
                  state.c_str());
    }
    return 0;
  }
  if ((sub == "install" || sub == "import") && argc >= 2) {
    auto res = client.Post(std::format("/v1/launchers/{}/{}", argv[1], sub));
    if (sub == "import") {
      json summary;
      if (!AwaitJob(client, res, summary)) return 1;
      std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
      return 0;
    }
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    if (sub == "install") {
      std::printf("installing: `mira launcher list` to check on it\n");
    }
    return 0;
  }
  if (sub == "open" && argc >= 2) {
    json body = json::object();
    for (int i = 2; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if ((arg == "--launch" || arg == "--install") && i + 1 < argc) {
        body["action"] = arg.substr(2);
        body["ref"] = argv[++i];
      } else {
        std::fprintf(stderr, "mira: unknown launcher option \"%s\"\n", argv[i]);
        return 2;
      }
    }
    auto res = client.Post(std::format("/v1/launchers/{}/open", argv[1]), body.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    return 0;
  }
  std::fprintf(stderr,
               "usage: mira launcher list\n"
               "       mira launcher install|import <battlenet|ubisoft|ea|office>\n"
               "       mira launcher open <id> [--launch REF | --install REF]\n");
  return 2;
}

// Reads one trimmed line from the terminal, after `prompt`.
std::string Prompt(const char* prompt) {
  std::fputs(prompt, stdout);
  std::string line;
  std::getline(std::cin, line);
  // A terminal paste routinely carries a trailing \r or spaces.
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  size_t start = 0;
  while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
  return line.substr(start);
}

// What each store wants pasted after signing in.
const char* CredentialPrompt(std::string_view store) {
  if (store == "epic") return "Paste the \"authorizationCode\" shown, or the whole page.\ncode (or pasted JSON): ";
  if (store == "gog") return "It redirects to a blank page. Paste that page's whole address-bar URL, or just its \"code\".\nurl or code: ";
  if (store == "amazon") return "It ends on an amazon.com page. Paste that page's whole address-bar URL.\nurl: ";
  if (store == "itch") return "Paste an API key.\napi key: ";
  return "Paste the _simpleauth_sess cookie value of a logged-in browser session.\nsession key: ";
}

int CmdStoreStatus(httplib::Client& client, const std::string& id) {
  auto res = client.Get(std::format("/v1/stores/{}/status", id));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  const json status = json::parse(res->body);
  const json& tool = status["tool"];
  if (!tool.value("installed", false)) {
    std::printf("%s: tool not installed, run \"mira store %s setup\"\n", id.c_str(), id.c_str());
    return 0;
  }
  std::printf("tool: installed (%s, %s) at %s\n", tool.value("source", "").c_str(), tool.value("version", "").c_str(),
              tool.value("path", "").c_str());
  if (!status.value("authenticated", false)) {
    std::printf("not authenticated, run \"mira store %s login\"\n", id.c_str());
  } else if (const std::string account = status.value("account", std::string()); !account.empty()) {
    std::printf("authenticated as %s\n", account.c_str());
  } else {
    std::puts("authenticated");
  }
  return 0;
}

int CmdStoreLogin(httplib::Client& client, const std::string& id, int argc, char** argv) {
  std::string credential = argc > 0 ? argv[0] : "";
  if (credential.empty()) {
    auto begun = client.Post(std::format("/v1/stores/{}/login/begin", id));
    if (!Ok(begun)) {
      PrintError(begun);
      return 1;
    }
    std::printf("Visit this URL and sign in:\n%s\n\n", json::parse(begun->body).value("url", "").c_str());
    credential = Prompt(CredentialPrompt(id));
    if (credential.empty()) {
      std::fprintf(stderr, "mira: nothing entered\n");
      return 2;
    }
  }
  auto res = client.Post(std::format("/v1/stores/{}/login", id), json{{"credential", credential}}.dump(),
                         "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  const std::string account = json::parse(res->body).value("account", std::string());
  std::puts(account.empty() ? "authenticated" : std::format("authenticated as {}", account).c_str());
  return 0;
}

int CmdStoreItchCollections(httplib::Client& client, int argc, char** argv) {
  const std::string_view action = argc > 0 ? argv[0] : "list";
  if (action == "add" && argc > 1) {
    auto res = client.Post("/v1/stores/itch/collections", json{{"link", argv[1]}}.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    const json added = json::parse(res->body);
    std::printf("added %s (%lld games)\n", added.value("title", std::string()).c_str(), added.value("games_count", 0LL));
    return 0;
  }
  if (action == "remove" && argc > 1) {
    auto res = client.Delete(std::format("/v1/stores/itch/collections/{}", argv[1]));
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("removed");
    return 0;
  }
  if (action != "list") {
    std::fprintf(stderr, "usage: mira store itch collections [list | add <link> | remove <id>]\n");
    return 2;
  }
  auto res = client.Get("/v1/stores/itch/collections");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  for (const json& collection : json::parse(res->body)) {
    std::printf("%-10lld %-5s %4lld games  %s\n", collection.value("id", 0LL),
                collection.value("own", false) ? "own" : "added", collection.value("games_count", 0LL),
                collection.value("title", std::string()).c_str());
  }
  return 0;
}

int CmdStoreHumble(httplib::Client& client, std::string_view action, int argc, char** argv) {
  if (action == "bundles") {
    auto res = client.Get("/v1/stores/humble/bundles");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    const json bundles = json::parse(res->body);
    if (bundles.empty()) {
      std::puts("(nothing found. Is humble-cli set up and signed in? Try `mira store humble status`)");
      return 0;
    }
    for (const json& bundle : bundles) {
      std::printf("%-24s %-8s %s\n", bundle.value("key", "").c_str(), bundle.value("claimed", false) ? "claimed" : "",
                  bundle.value("name", "").c_str());
    }
    return 0;
  }
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira store humble download <bundle-key> [item-numbers]\n");
    return 2;
  }
  json body = {{"bundle_key", argv[0]}};
  if (argc > 1) body["item_numbers"] = argv[1];
  json result;
  if (!AwaitJob(client, client.Post("/v1/stores/humble/download", body.dump(), "application/json"), result)) return 1;
  std::printf("downloaded into %s\n", result.value("path", std::string()).c_str());
  return 0;
}

// mira store [list] | mira store <id> status|setup|login|logout|import
int CmdStore(int argc, char** argv) {
  auto client = Connect();
  if (argc == 0 || std::string_view(argv[0]) == "list") {
    auto res = client.Get("/v1/stores");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    for (const json& store : json::parse(res->body)) {
      std::printf("%-8s %-16s %s\n", store.value("id", "").c_str(), store.value("name", "").c_str(),
                  store.value("tool_name", "").c_str());
    }
    return 0;
  }
  const std::string id = argv[0];
  const std::string_view action = argc > 1 ? argv[1] : "status";
  if (action == "status") return CmdStoreStatus(client, id);
  if (action == "login") return CmdStoreLogin(client, id, argc - 2, argv + 2);
  if (id == "itch" && action == "collections") return CmdStoreItchCollections(client, argc - 2, argv + 2);
  if (id == "humble" && (action == "bundles" || action == "download")) {
    return CmdStoreHumble(client, action, argc - 2, argv + 2);
  }
  if (action == "logout") {
    auto res = client.Post(std::format("/v1/stores/{}/logout", id));
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("logged out");
    return 0;
  }
  if (action == "setup" || action == "import") {
    json result;
    if (!AwaitJob(client, client.Post(std::format("/v1/stores/{}/{}", id, action)), result)) return 1;
    if (action == "setup") {
      std::printf("installed %s\n", result.value("tag", std::string()).c_str());
    } else {
      std::printf("added: %lld  updated: %lld\n", result.value("added", 0LL), result.value("updated", 0LL));
    }
    return 0;
  }
  std::fprintf(stderr,
               "usage: mira store [list]\n"
               "       mira store <epic|gog|itch|amazon|humble> status|setup|login [credential]|logout|import\n"
               "       mira store itch collections [list | add <link> | remove <id>]\n"
               "       mira store humble bundles | download <bundle-key> [item-numbers]\n"
               "       (installing is source-generic: mira library install <source> <id>)\n");
  return 2;
}

int CmdLibraryList(int argc, char** argv) {
  auto client = Connect();
  const std::string path =
      argc > 0 ? std::format("/v1/library?source={}", argv[0]) : std::string("/v1/library");
  auto res = client.Get(path.c_str());
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json entries = json::parse(res->body);
  if (entries.empty()) {
    std::puts("(nothing found. Is the source configured and authenticated? Try `mira store epic status`)");
    return 0;
  }
  for (const json& entry : entries) {
    std::printf("%-8s %-40s %-12s %s\n", entry.value("source", "").c_str(),
               entry.value("ref", "").c_str(),
               entry.value("installed", false) ? "[installed]" : "",
               entry.value("title", "").c_str());
  }
  return 0;
}

int CmdLibraryInstallOrUpdate(int argc, char** argv, bool is_update) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: mira library %s <source> <ref>\n", is_update ? "update" : "install");
    return 2;
  }
  auto client = Connect();
  json body = {{"source", argv[0]}, {"ref", argv[1]}};
  auto res = client.Post(is_update ? "/v1/library/update" : "/v1/library/install", body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::printf("%s: watch `mira watch` for library.install.finished\n", is_update ? "updating" : "installing");
  return 0;
}

int CmdLibrary(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "install") {
    return CmdLibraryInstallOrUpdate(argc - 1, argv + 1, false);
  }
  if (argc > 0 && std::string_view(argv[0]) == "update") {
    return CmdLibraryInstallOrUpdate(argc - 1, argv + 1, true);
  }
  if (argc > 0 && std::string_view(argv[0]) == "relocate") {
    if (argc > 1) {
      std::fprintf(stderr, "mira library relocate takes no options; it moves every tracked game\n");
      return 2;
    }
    return CmdLibraryRelocate();
  }
  // `mira library` / `mira library <source>` both list.
  return CmdLibraryList(argc, argv);
}

int CmdDesktopEntriesList() {
  auto client = Connect();
  auto res = client.Get("/v1/desktop-entries/candidates");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json candidates = json::parse(res->body);
  if (candidates.empty()) {
    std::puts("(no candidates: every already-installed .desktop entry is either already a game, "
              "Mira's own, or covered by another importer)");
    return 0;
  }
  for (const json& c : candidates) {
    std::printf("%-40s %s\n", c.value("id", "").c_str(), c.value("name", "").c_str());
  }
  return 0;
}

int CmdDesktopEntriesImport(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira desktop-entries import <id> [<id>...]\n");
    return 2;
  }
  json ids = json::array();
  for (int i = 0; i < argc; ++i) ids.push_back(argv[i]);

  auto client = Connect();
  auto res = client.Post("/v1/desktop-entries/import", json{{"ids", ids}}.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json summary = json::parse(res->body);
  std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
  return 0;
}

int CmdDesktopEntries(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "list") return CmdDesktopEntriesList();
  if (argc > 0 && std::string_view(argv[0]) == "import") return CmdDesktopEntriesImport(argc - 1, argv + 1);
  std::fprintf(stderr, "usage: mira desktop-entries [list | import <id> [<id>...]]\n");
  return 2;
}

int CmdGameModeStatus() {
  auto client = Connect();
  auto res = client.Get("/v1/gamemode/status");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json status = json::parse(res->body);
  std::printf("installed: %s  daemon running: %s\n", status.value("installed", false) ? "yes" : "no",
             status.value("daemon_running", false) ? "yes" : "no");
  return 0;
}

int CmdGameMode(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "status") return CmdGameModeStatus();
  std::fprintf(stderr, "usage: mira gamemode status\n");
  return 2;
}

int CmdTricks(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: mira tricks <id> <verb>\n"
                 "  runs a winetricks verb (e.g. corefonts, vcrun2019, win10) against this\n"
                 "  game's own prefix. Runs in the background; watch `mira watch` for\n"
                 "  tricks.finished/.failed.\n");
    return 2;
  }
  const std::string id = argv[0];
  const std::string verb = argv[1];

  auto client = Connect();
  const json body = {{"verb", verb}};
  auto res = client.Post(std::format("/v1/games/{}/tricks", id), body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running: watch `mira watch` for tricks.finished/.failed");
  return 0;
}

int CmdMetadata(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira metadata <id> [--refresh]\n"
                 "       mira metadata <id> --matches [QUERY]\n"
                 "       mira metadata <id> --wrong\n"
                 "       mira metadata <id> --match N | --match-id STEAMGRIDDB_ID\n"
                 "  prints cached cover-art/store-info JSON; --refresh re-fetches first.\n"
                 "  --matches lists SteamGridDB's matches for the game's name; --match N takes\n"
                 "  art from the Nth of them instead (0 goes back to the top match). --wrong\n"
                 "  says the art is for the wrong game and moves to the next match.\n");
    return 2;
  }
  const std::string id = argv[0];
  const std::string_view flag = argc > 1 ? argv[1] : "";
  auto client = Connect();

  if (flag == "--refresh") {
    auto posted = client.Post(std::format("/v1/games/{}/metadata/refresh", id));
    if (!Ok(posted)) {
      PrintError(posted);
      return 1;
    }
    std::puts("fetching: watch `mira watch` for game.metadata_ready/.metadata_failed");
    return 0;
  }

  if (flag == "--wrong") {
    auto posted = client.Post(std::format("/v1/games/{}/metadata/wrong-match", id));
    if (!Ok(posted)) {
      PrintError(posted);
      return 1;
    }
    const json match = json::parse(posted->body).value("match", json::object());
    std::printf("switched to \"%s\" (SteamGridDB %lld), fetching its art\n", match.value("name", "").c_str(),
                static_cast<long long>(match.value("id", std::int64_t{0})));
    return 0;
  }

  if (flag == "--matches" || flag == "--match" || flag == "--match-id") {
    std::int64_t chosen_id = -1;
    if (flag == "--match-id" && argc > 2) chosen_id = std::atoll(argv[2]);
    if (flag == "--matches" || flag == "--match") {
      httplib::Params params;
      if (flag == "--matches" && argc > 2) params.emplace("q", argv[2]);
      auto res = client.Get(std::format("/v1/games/{}/metadata/matches", id), params, httplib::Headers{});
      if (!Ok(res)) {
        PrintError(res);
        return 1;
      }
      const json body = json::parse(res->body);
      const json& matches = body["matches"];
      if (flag == "--matches") {
        const std::int64_t chosen = body.value("chosen", std::int64_t{0});
        int n = 0;
        for (const json& match : matches) {
          const std::int64_t match_id = match.value("id", std::int64_t{0});
          const bool active = chosen == 0 ? n == 0 : chosen == match_id;
          std::printf("%s%2d  %-10lld %s\n", active ? "*" : " ", ++n, static_cast<long long>(match_id),
                      match.value("name", "").c_str());
        }
        if (matches.empty()) std::puts("no matches");
        return 0;
      }
      const int n = argc > 2 ? std::atoi(argv[2]) : -1;
      if (n == 0) {
        chosen_id = 0;
      } else if (n < 1 || n > static_cast<int>(matches.size())) {
        std::fprintf(stderr, "mira: pick a number from `mira metadata %s --matches`\n", id.c_str());
        return 2;
      } else {
        chosen_id = matches[n - 1].value("id", std::int64_t{0});
      }
    }
    if (chosen_id < 0) {
      std::fprintf(stderr, "usage: mira metadata <id> --match-id STEAMGRIDDB_ID\n");
      return 2;
    }
    auto posted = client.Post(std::format("/v1/games/{}/metadata/match", id),
                              json{{"steamgriddb_id", chosen_id}}.dump(), "application/json");
    if (!Ok(posted)) {
      PrintError(posted);
      return 1;
    }
    std::puts("fetching art from the chosen match");
    return 0;
  }

  auto res = client.Get(std::format("/v1/games/{}/metadata", id));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdList(int argc, char** argv) {
  std::string status_filter, tag_filter;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--status" && i + 1 < argc) status_filter = argv[++i];
    else if (arg == "--tag" && i + 1 < argc) tag_filter = argv[++i];
  }
  auto client = Connect();
  std::string path = "/v1/games";
  std::string query;
  if (!status_filter.empty()) query += "status=" + status_filter + "&";
  if (!tag_filter.empty()) query += "tag=" + tag_filter + "&";
  if (!query.empty()) {
    query.pop_back();  // trailing '&'
    path += "?" + query;
  }
  auto res = client.Get(path);
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json games = json::parse(res->body);
  if (games.empty()) {
    std::puts("(no games)");
    return 0;
  }
  for (const json& game : games) {
    std::printf("%-24s %-10s %-8s %s%s\n", game.value("id", "").c_str(),
               game.value("status", "").c_str(),
               game.value("reviewed", false) ? "" : "[unreviewed]",
               game.value("name", "").c_str(),
               game.value("platform", "") == "windows" ? "  (windows)" : "");
  }
  return 0;
}

int CmdShow(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira show <id> [--effective]\n");
    return 2;
  }
  const std::string id = argv[0];
  const bool effective = argc > 1 && std::string_view(argv[1]) == "--effective";

  auto client = Connect();
  auto res = client.Get(effective ? std::format("/v1/games/{}/config", id)
                                  : std::format("/v1/games/{}", id));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdSet(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira set <id> [--name N] [--exe PATH] [--args ARGS]\n"
                 "                     [--runner kind:name] [--data-dir PATH]\n"
                 "                     [--env KEY=VALUE]...\n"
                 "                     [--tag NAME]... [--untag NAME]...\n"
                 "                     [--override dotted.key=VALUE]... [--unset dotted.key]...\n"
                 "  --tag hidden leaves this game out of `mira list`/GET /v1/games by\n"
                 "  default (still reachable via --tag hidden or `mira show`).\n");
    return 2;
  }
  const std::string id = argv[0];
  json patch = json::object();     // -> PATCH /v1/games/{id}: this game's own fields
  json overrides = json::object(); // -> PATCH /v1/games/{id}/config: overrides of global settings
  json env = json::object();
  std::vector<std::string> add_tags, remove_tags;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (arg == "--name") {
      patch["name"] = next();
    } else if (arg == "--exe") {
      patch["exe_path"] = next();
    } else if (arg == "--args") {
      patch["args"] = next();
    } else if (arg == "--runner") {
      patch["runner_ref"] = next();
    } else if (arg == "--data-dir") {
      patch["data_dir"] = next();
    } else if (arg == "--env") {
      const std::string kv = next();
      if (const auto eq = kv.find('='); eq != std::string::npos) {
        env[kv.substr(0, eq)] = kv.substr(eq + 1);
      }
    } else if (arg == "--tag") {
      add_tags.push_back(next());
    } else if (arg == "--untag") {
      remove_tags.push_back(next());
    } else if (arg == "--override") {
      const std::string kv = next();
      if (const auto eq = kv.find('='); eq != std::string::npos) {
        json value = json::parse(kv.substr(eq + 1), nullptr, false);
        overrides[kv.substr(0, eq)] = value.is_discarded() ? json(kv.substr(eq + 1)) : value;
      }
    } else if (arg == "--unset") {
      overrides[next()] = nullptr;
    }
  }
  if (!env.empty()) patch["env"] = env;

  auto client = Connect();
  bool changed = false;

  // --tag/--untag go through the batch PATCH, which adds and removes on the server instead of
  // replacing the whole list from one read here.
  if (!add_tags.empty() || !remove_tags.empty()) {
    const json body = {{"ids", json::array({id})}, {"add_tags", add_tags}, {"remove_tags", remove_tags}};
    auto res = client.Patch("/v1/games", body.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    changed = true;
  }

  if (!patch.empty()) {
    auto res = client.Patch(std::format("/v1/games/{}", id), patch.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    changed = true;
  }
  if (!overrides.empty()) {
    auto res =
        client.Patch(std::format("/v1/games/{}/config", id), overrides.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    changed = true;
  }

  std::puts(changed ? "updated" : "nothing to do");
  return 0;
}

int CmdConfig(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira config get|set|list|reset [args...]\n");
    return 2;
  }
  const std::string_view sub = argv[0];
  auto client = Connect();

  if (sub == "list") {
    auto res = client.Get("/v1/config/schema");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    for (const json& entry : json::parse(res->body)) {
      std::printf("%-32s %-8s %-9s %s\n", entry.value("key", "").c_str(),
                 entry.value("type", "").c_str(), entry.value("scope", "").c_str(),
                 entry.value("doc", "").c_str());
    }
    return 0;
  }

  if (sub == "get") {
    if (argc < 2) {
      std::fprintf(stderr, "usage: mira config get <key>\n");
      return 2;
    }
    auto res = client.Get("/v1/config");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    json document = json::parse(res->body);
    json::json_pointer pointer;
    std::string flat(argv[1]);
    for (char& c : flat) {
      if (c == '.') c = '/';
    }
    pointer = json::json_pointer("/" + flat);
    if (!document.contains(pointer)) {
      std::fprintf(stderr, "mira: unknown setting \"%s\"\n", argv[1]);
      return 1;
    }
    std::cout << document[pointer].dump() << '\n';
    return 0;
  }

  if (sub == "set") {
    if (argc < 3) {
      std::fprintf(stderr, "usage: mira config set <key> <value>\n");
      return 2;
    }
    json value = json::parse(argv[2], nullptr, false);
    if (value.is_discarded()) value = std::string(argv[2]);  // bare strings need no quoting

    std::string flat(argv[1]);
    json patch = json::object();
    json* cursor = &patch;
    size_t start = 0;
    while (true) {
      const size_t dot = flat.find('.', start);
      const std::string segment = flat.substr(start, dot == std::string::npos ? dot : dot - start);
      if (dot == std::string::npos) {
        (*cursor)[segment] = value;
        break;
      }
      cursor = &(*cursor)[segment];
      start = dot + 1;
    }
    auto res = client.Patch("/v1/config", patch.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("ok");
    return 0;
  }

  if (sub == "reset") {
    const std::string path = argc > 1 ? std::format("/v1/config/reset?key={}", argv[1])
                                       : "/v1/config/reset";
    auto res = client.Post(path);
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("ok");
    return 0;
  }

  std::fprintf(stderr, "mira: unknown config subcommand \"%.*s\"\n", static_cast<int>(sub.size()),
              sub.data());
  return 2;
}

int CmdWatch() {
  auto client = Connect();
  client.set_read_timeout(std::chrono::hours(24 * 365));  // events can be arbitrarily far apart
  std::string buffer;
  auto res = client.Get("/v1/events", [&](const char* data, size_t length) {
    buffer.append(data, length);
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
      std::cout << buffer.substr(0, pos) << "\n---\n";
      std::cout.flush();
      buffer.erase(0, pos + 2);
    }
    return true;
  });
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  return 0;
}

int CmdDaemon(int argc, char** argv, const char* self) {
  // `mira daemon` is a convenience that execs mirad alongside this binary (or
  // on PATH), forwarding any remaining arguments. It does not talk to the
  // API, since there is nothing listening yet.
  std::filesystem::path candidate = std::filesystem::path(self).parent_path() / "mirad";
  std::vector<char*> exec_argv;
  const std::string exe = std::filesystem::exists(candidate) ? candidate.string() : "mirad";
  exec_argv.push_back(const_cast<char*>(exe.c_str()));
  for (int i = 0; i < argc; ++i) exec_argv.push_back(argv[i]);
  exec_argv.push_back(nullptr);
  execvp(exe.c_str(), exec_argv.data());
  std::perror("mira: failed to start mirad");
  return 1;
}

int CmdSetup(int argc, char** argv) {
  // Local only, like `daemon`: writes user-level files, no mirad involved.
  bool remove = false;
  bool uninstall = false;
  bool enable_service = false;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--remove") {
      remove = true;
    } else if (arg == "--uninstall") {
      remove = true;
      uninstall = true;
    } else if (arg == "--enable-service") {
      enable_service = true;
    } else {
      std::fprintf(stderr, "usage: mira setup [--enable-service] [--remove] [--uninstall]\n");
      return 2;
    }
  }

  const mira::setup::SetupPaths paths = mira::setup::DefaultPaths();
  const char* appimage_env = std::getenv("APPIMAGE");
  if (uninstall) {
    // Launched from the menu there's no terminal to ask in, so use whichever
    // dialog tool is installed; refuse if there's none.
    constexpr const char* kQuestion = "Uninstall Mira? Your games, settings and prefixes are kept.";
    if (!isatty(STDIN_FILENO)) {
      const std::string ask = std::format(
          "if command -v zenity >/dev/null; then zenity --question --text='{0}'; "
          "elif command -v kdialog >/dev/null; then kdialog --yesno '{0}'; "
          "elif command -v yad >/dev/null; then yad --text='{0}'; "
          "else exit 1; fi 2>/dev/null",
          kQuestion);
      if (std::system(ask.c_str()) != 0) return 1;
    } else {
      std::printf("%s [y/N] ", kQuestion);
      std::string answer;
      std::getline(std::cin, answer);
      if (answer != "y" && answer != "Y") return 1;
    }
  }
  if (remove) {
    std::system("systemctl --user disable --now mirad.service >/dev/null 2>&1");
    const auto removed = mira::setup::Remove(paths);
    for (const auto& path : *removed) std::printf("removed %s\n", path.c_str());
    if (removed->empty()) std::puts("nothing to remove");
    std::system("systemctl --user daemon-reload >/dev/null 2>&1");
    if (uninstall) {
      std::error_code ec;
      std::filesystem::remove_all(mira::paths::Home() / ".cache/mira-appimage", ec);
      if (appimage_env && *appimage_env && std::filesystem::remove(appimage_env, ec)) {
        std::printf("removed %s\n", appimage_env);
      }
    }
    return 0;
  }

  const char* appimage = appimage_env;
  if (!appimage || !*appimage) {
    std::fprintf(stderr, "mira: run this from the AppImage: ./Mira-x86_64.AppImage setup\n");
    return 1;
  }
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe");
  const std::filesystem::path icon = self.parent_path().parent_path() / "share/icons/hicolor/256x256/apps/mira.png";

  const auto written = mira::setup::Install(paths, appimage, icon);
  if (!written) {
    std::fprintf(stderr, "mira: %s\n", written.error().message.c_str());
    return 1;
  }
  for (const auto& path : *written) std::printf("wrote %s\n", path.c_str());
  std::system("systemctl --user daemon-reload >/dev/null 2>&1");
  std::system("update-desktop-database -q ~/.local/share/applications >/dev/null 2>&1");

  const char* path_env = std::getenv("PATH");
  if (!path_env || std::string(path_env).find(paths.bin_dir.string()) == std::string::npos) {
    std::printf("note: %s isn't on PATH -- add it to use `mira` from a shell\n", paths.bin_dir.c_str());
  }
  if (enable_service) {
    if (std::system("systemctl --user enable --now mirad.service") != 0) {
      std::fprintf(stderr, "mira: couldn't enable mirad.service\n");
      return 1;
    }
    std::puts("mirad.service enabled");
  } else {
    std::puts("mirad starts with the GUI; `systemctl --user enable --now mirad` keeps it running instead");
  }
  return 0;
}

void PrintUsage() {
  std::puts(
      "usage: mira <command> [args...]\n"
      "\n"
      "commands:\n"
      "  status                 check whether mirad is reachable\n"
      "  daemon [args...]       exec mirad in the foreground\n"
      "  setup [--enable-service] [--remove|--uninstall]   add mira to PATH, the app\n"
      "                         menu and systemd; --uninstall also deletes the AppImage\n"
      "                         (run from the AppImage: Mira-x86_64.AppImage setup)\n"
      "  scan                   scan all library roots now\n"
      "  runners                list installed Proton/Wine builds\n"
      "  runners sources [proton|wine]            list where builds download from\n"
      "  runners catalog [--kind K] [--source ID] list downloadable versions\n"
      "  runners download --kind K --tag TAG [--source ID]   download and install one\n"
      "  runners updates | update <kind:name>     list or install newer builds\n"
      "  runners tools [install umu|winetricks]   check or install runner helpers\n"
      "  launch <id>            launch a game (or fire steam://rungameid for a\n"
      "                         Steam game, depending on steam.launch_mode)\n"
      "  stop <id>              stop a running game\n"
      "  run <id> --exe PATH [--args ARGS]        run an arbitrary exe in this\n"
      "                         game's prefix, which is how you run a needs_install game's\n"
      "                         installer\n"
      "  install <id> [--interactive] [--installer PATH]   run a needs_install game's\n"
      "                         installer (silent for Inno/NSIS, otherwise shown)\n"
      "  install <id> --info|--progress           describe the installer / check progress\n"
      "  finish-install <id>    mark a needs_install game ready after installing\n"
      "  list [--status S] [--tag T]        list games (hidden-tagged ones excluded\n"
      "                         by default; --tag hidden lists exactly those)\n"
      "  show <id> [--effective] show one game, or its resolved settings\n"
      "  set <id> [flags...]    correct a game's auto-detected configuration\n"
      "  remove <id> [--delete-files] [--delete-prefix] [--delete-metadata] [--purge]\n"
      "  add <install_path> <exe_path> [--name N] [--platform windows|native] [--installer]\n"
      "  relocate <id>           move a game's files into Mira's canonical layout\n"
      "  library relocate        relocate every tracked game (see `mira relocate`)\n"
      "  steam scan             detect installed Steam games\n"
      "  steam status online|invisible   set your Steam friends status\n"
      "  lutris import          import games from Lutris's own database\n"
      "  launcher list|install|import|open   Battle.net, Ubisoft Connect and EA app\n"
      "  store [list]           list the stores: Epic, GOG, itch, Amazon Games, Humble Bundle\n"
      "  store <id> status|setup|login [credential]|logout|import   one store's tool and account\n"
      "  store itch collections [list|add <link>|remove <id>]\n"
      "  store humble bundles | download <bundle-key> [item-numbers]\n"
      "  desktop-entries list    list already-installed .desktop entries that could become games\n"
      "  desktop-entries import <id> [<id>...]   add the picked ones (covers Flatpak apps too)\n"
      "  gamemode status         check whether GameMode is installed/running\n"
      "  metadata <id> [--refresh]                cached cover-art/store info\n"
      "  tricks <id> <verb>     run a winetricks verb against this game's prefix\n"
      "  config get|set|list|reset [args...]\n"
      "  watch                  tail the event stream\n");
}

}  // namespace

int Dispatch(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string_view command = argv[1];
  char** rest = argv + 2;
  const int rest_argc = argc - 2;

  if (command == "--help" || command == "-h" || command == "help") {
    PrintUsage();
    return 0;
  }
  if (command == "status") return CmdStatus();
  if (command == "scan") return CmdScan();
  if (command == "runners") return CmdRunners(rest_argc, rest);
  if (command == "launch") return CmdLaunch(rest_argc, rest);
  if (command == "stop") return CmdStop(rest_argc, rest);
  if (command == "run") return CmdRun(rest_argc, rest);
  if (command == "install") return CmdInstall(rest_argc, rest);
  if (command == "finish-install") return CmdFinishInstall(rest_argc, rest);
  if (command == "relocate") return CmdRelocate(rest_argc, rest);
  if (command == "remove") return CmdRemove(rest_argc, rest);
  if (command == "add") return CmdAdd(rest_argc, rest);
  if (command == "steam") return CmdSteam(rest_argc, rest);
  if (command == "lutris") return CmdLutris(rest_argc, rest);
  if (command == "launcher") return CmdLauncher(rest_argc, rest);
  if (command == "store") return CmdStore(rest_argc, rest);
  if (command == "library") return CmdLibrary(rest_argc, rest);
  if (command == "desktop-entries") return CmdDesktopEntries(rest_argc, rest);
  if (command == "gamemode") return CmdGameMode(rest_argc, rest);
  if (command == "metadata") return CmdMetadata(rest_argc, rest);
  if (command == "tricks") return CmdTricks(rest_argc, rest);
  if (command == "setup") return CmdSetup(rest_argc, rest);
  if (command == "daemon") return CmdDaemon(rest_argc, rest, argv[0]);
  if (command == "list") return CmdList(rest_argc, rest);
  if (command == "show") return CmdShow(rest_argc, rest);
  if (command == "set") return CmdSet(rest_argc, rest);
  if (command == "config") return CmdConfig(rest_argc, rest);
  if (command == "watch") return CmdWatch();

  std::fprintf(stderr, "mira: unknown command \"%.*s\"\n", static_cast<int>(command.size()),
              command.data());
  PrintUsage();
  return 2;
}

int main(int argc, char** argv) {
  // A reply that isn't the JSON it should be throws out of whichever command read it.
  try {
    return Dispatch(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "mira: unexpected reply from mirad: %s\n", error.what());
    return 1;
  }
}
