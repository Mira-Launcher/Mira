#include "api/Routes.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#include <format>
#include <fstream>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "library/GamePatch.h"
#include "config/Resolver.h"
#include "core/Strings.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

}  // namespace

void RegisterGameRoutes(httplib::Server& http, Services& s) {
  // --- games ----------------------------------------------------------------

  // Games tagged "hidden" are left out unless a tag is asked for, or include_hidden=true.
  http.Get("/v1/games", [&s](const Request& req, Response& res) {
    std::vector<model::Game> all = s.games.All();
    json out = json::array();
    const auto status_filter = req.params.find("status");
    const auto tag_filter = req.params.find("tag");
    const bool include_hidden = BoolParam(req, "include_hidden");
    for (const model::Game& game : all) {
      if (status_filter != req.params.end() &&
          status_filter->second != model::ToString(game.status)) {
        continue;
      }
      if (tag_filter != req.params.end()) {
        if (!std::ranges::contains(game.tags, tag_filter->second)) continue;
      } else if (!include_hidden && std::ranges::contains(game.tags, std::string("hidden"))) {
        continue;
      }
      out.push_back(s.Record(game));
    }
    SendJson(res, std::move(out));
  });

  http.Get(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    json body = s.Record(*game);
    // What an empty runner_ref runs with, so an editor can show that runner's options.
    model::Game unpinned = *game;
    unpinned.runner_ref.clear();
    body["default_runner"] = runner::RunnerRegistry(s.config).ResolveRef(unpinned);
    SendJson(res, body);
  });

  // The tail of mira-run's log for this game. No log yet is an empty list.
  http.Get(R"(/v1/games/([^/]+)/log)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    int requested_lines = 200;
    if (auto it = req.params.find("lines"); it != req.params.end()) {
      const std::string& raw = it->second;
      if (std::from_chars(raw.data(), raw.data() + raw.size(), requested_lines).ec != std::errc() || requested_lines < 1) {
        return SendError(res, 400, "invalid_param", "?lines= must be a whole number, 1 or more");
      }
    }

    const std::filesystem::path log_file = s.games.Dir() / "logs" / std::format("{}.log", game->id);
    std::ifstream in(log_file, std::ios::binary);
    if (!in) return SendJson(res, {{"lines", json::array()}});

    // Only the end of the file is read; logs can be up to launch.log_max_mb.
    constexpr std::streamoff kMaxTailBytes = 4 * 1024 * 1024;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(size > kMaxTailBytes ? size - kMaxTailBytes : 0);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::string> all_lines = strings::Split(content, '\n');
    if (!all_lines.empty() && all_lines.back().empty()) all_lines.pop_back();  // trailing newline
    const std::size_t take = std::min(all_lines.size(), static_cast<std::size_t>(requested_lines));
    json out = json::array();
    for (std::size_t i = all_lines.size() - take; i < all_lines.size(); ++i) out.push_back(all_lines[i]);
    SendJson(res, {{"lines", out}});
  });

  http.Patch(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");
    if (const auto problem = library::GamePatchProblem(patch)) return SendError(res, 400, "invalid_body", *problem);

    auto result = s.games.Update(id, [&](model::Game& game) { game = library::ParseGamePatch(game, patch); });
    if (!result) return SendStoreError(res, result.error());
    s.SyncDesktopEntry(id);
    s.events.Publish("game.updated", s.Record(*result));
    SendJson(res, s.Record(*result));
  });

  // One save, one menu sync and one event for any number of games, so a
  // multi-select doesn't cost a request (and a full rewrite) per game.
  http.Patch("/v1/games", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    const auto add_tags = body.is_object() ? StringList(body, "add_tags") : std::nullopt;
    const auto remove_tags = body.is_object() ? StringList(body, "remove_tags") : std::nullopt;
    const json config = body.is_object() ? body.value("config", json::object()) : json();
    if (!ids || !add_tags || !remove_tags || !config.is_object()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"ids": [...], "add_tags"?: [...], "remove_tags"?: [...], "config"?: {...}})");
    }
    if (auto problem = library::ValidateOverridesPatch(config)) return SendError(res, 400, "invalid_setting", *problem);

    auto updated = s.games.UpdateMany(*ids, [&](model::Game& game) {
      const std::vector<std::string> old_tags = game.tags;
      const json old_overrides = game.overrides;
      std::erase_if(game.tags, [&](const std::string& tag) { return std::ranges::contains(*remove_tags, tag); });
      for (const std::string& tag : *add_tags) {
        if (!std::ranges::contains(game.tags, tag)) game.tags.push_back(tag);
      }
      library::ApplyOverridesPatch(game, config);
      return game.tags != old_tags || game.overrides != old_overrides;
    });
    if (!updated) return SendStoreError(res, updated.error());

    json games = json::array();
    for (const model::Game& game : *updated) games.push_back(s.Record(game));
    if (!updated->empty()) {
      // Tags never change a menu entry; only an override can.
      if (!config.empty()) {
        for (const model::Game& game : *updated) s.SyncDesktopEntry(game.id);
      }
      s.events.Publish("games.updated", {{"games", games}});
    }
    SendJson(res, {{"games", std::move(games)}});
  });

  http.Get(R"(/v1/games/([^/]+)/config)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    config::Resolver resolver(s.config, game->overrides);
    SendJson(res, resolver.EffectiveDocument());
  });

  http.Patch(R"(/v1/games/([^/]+)/config)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded() || !patch.is_object()) {
      return SendError(res, 400, "invalid_json", "expected a flat {\"dotted.key\": value} object");
    }
    if (auto problem = library::ValidateOverridesPatch(patch)) {
      return SendError(res, 400, "invalid_setting", *problem);
    }
    auto result =
        s.games.Update(id, [&](model::Game& game) { library::ApplyOverridesPatch(game, patch); });
    if (!result) return SendStoreError(res, result.error());
    // An override can turn desktop_entries.enabled off for this game.
    s.SyncDesktopEntry(id);
    s.events.Publish("game.updated", s.Record(*result));
    SendJson(res, s.Record(*result));
  });

  http.Delete(R"(/v1/games/([^/]+))", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    const bool purge = BoolParam(req, "purge");
    const auto flag = [&](const char* name) {
      return purge || BoolParam(req, name);
    };
    const auto folders_lock = s.games.LockFolders();
    if (auto deleted = s.DeleteGameData(*game, flag("delete_files"), flag("delete_prefix"), flag("delete_metadata"));
        !deleted) {
      return SendError(res, deleted.error().code == "game_running" ? 409 : 400, deleted.error());
    }

    auto result = s.games.Remove(req.matches[1]);
    if (!result) return SendStoreError(res, result.error());
    s.SyncDesktopEntry(req.matches[1]);
    s.events.Publish("game.removed", {{"id", req.matches[1].str()}});
    SendJson(res, json::object());
  });

  // DELETE /v1/games/{id} for many games, with one save, menu sync and event.
  // A game whose files can't be deleted stays in the library.
  http.Post("/v1/games/delete", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    if (!ids) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"ids": [...], "delete_files"?, "delete_prefix"?, "delete_metadata"?})");
    }
    const bool purge = body.value("purge", false);
    const bool files = purge || body.value("delete_files", false);
    const bool prefix = purge || body.value("delete_prefix", false);
    const bool metadata = purge || body.value("delete_metadata", false);

    s.StartJob(req, res, "delete", "", "Removing games", [&s, ids = *ids, files, prefix, metadata](
                                                            JobRegistry::Progress& progress) -> Result<json> {
      std::vector<std::string> deletable;
      json failed = json::array();
      auto folders_lock = s.games.LockFolders();
      int done = 0;
      for (const std::string& id : ids) {
        progress.Report(done++, static_cast<int>(ids.size()));
        const auto game = s.games.Find(id);
        if (!game) continue;
        if (auto deleted = s.DeleteGameData(*game, files, prefix, metadata); !deleted) {
          failed.push_back(BatchFailure(id, deleted.error()));
          continue;
        }
        deletable.push_back(id);
      }
      auto removed = s.games.RemoveMany(deletable);
      folders_lock.unlock();
      if (!removed) return std::unexpected(removed.error());
      if (!removed->empty()) {
        for (const std::string& removed_id : *removed) s.SyncDesktopEntry(removed_id);
        s.events.Publish("games.removed", {{"ids", *removed}});
      }
      return json{{"removed", *removed}, {"failed", std::move(failed)}};
    });
  });

  // --- manual add -----------------------------------------------------------

  http.Post("/v1/games/manual", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("install_path") || !body["install_path"].is_string() ||
        !body.contains("exe_path") || !body["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"install_path": "...", "exe_path": "...", "name"?, "platform"?, "is_installer"?})");
    }
    const std::filesystem::path install_path = body["install_path"].get<std::string>();
    const std::string exe_path = body["exe_path"];
    const bool is_installer = body.value("is_installer", false);

    model::Platform platform;
    if (body.contains("platform") && body["platform"].is_string()) {
      platform = model::PlatformFromString(body["platform"].get<std::string>());
    } else {
      const std::string ext = strings::ToLower(std::filesystem::path(exe_path).extension().string());
      const bool windows = ext == ".exe" || ext == ".msi" || ext == ".bat" || ext == ".cmd";
      platform = windows ? model::Platform::Windows : model::Platform::Native;
    }

    model::Game game;
    const auto existing = s.games.FindByInstallPath(install_path.string());
    if (existing) game = *existing;
    game.name = body.value("name", strings::CleanGameName(install_path.filename().string()));
    game.id = existing ? game.id : s.games.NextId(game.name);
    game.source = "manual";
    game.install_path = install_path.string();
    game.exe_path = exe_path;
    game.args = body.value("args", std::string());
    game.platform = platform;
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;
    // An installer needs the prefix too: it installs the game into it.
    if (platform != model::Platform::Windows) {
      game.data_dir.clear();
    } else if (game.data_dir.empty()) {
      game.data_dir = library::PrefixDir(s.config, s.games, game).string();
    }

    if (is_installer) {
      // An installer isn't the game, so it isn't provisioned or launchable yet.
      game.status = model::GameStatus::NeedsInstall;
      game.last_error = "This is an installer, not the game itself. Install it to play.";
    } else {
      game.status = model::GameStatus::Ready;
      game.last_error.clear();
    }

    auto result = s.games.Upsert(game);
    if (!result) return SendError(res, 500, result.error());

    if (game.status == model::GameStatus::Ready && game.platform == model::Platform::Windows) {
      const runner::RunnerRegistry provisioner(s.config);
      const model::Game provisioned = provisioner.ProvisionGame(game);
      auto saved = s.games.Update(game.id, [&](model::Game& g) {
        g.runner_ref = provisioned.runner_ref;
        g.status = provisioned.status;
        g.last_error = provisioned.last_error;
      });
      if (saved) game = *saved;
    }

    s.SyncDesktopEntry(game.id);
    if (!existing) s.fetches.Enqueue(s.config, s.events, game);
    s.events.Publish(existing ? "game.updated" : "game.added", s.Record(game));
    SendJson(res, s.Record(game));
  });
}

}  // namespace mira::api
