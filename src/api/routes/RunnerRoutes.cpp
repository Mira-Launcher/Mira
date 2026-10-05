#include "api/Routes.h"

#include <algorithm>
#include <filesystem>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "runner/ProtonRunner.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "library/SourceRemoval.h"
#include "runner/Downloader.h"
#include "runner/RunnerRegistry.h"
#include "runner/RunnerUpdates.h"
#include "runner/Winetricks.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// Deletes `target` only if it resolves (symlinks included) inside one of `roots`.
Result<void> DeleteUnderRoot(const std::string& target, const std::vector<std::filesystem::path>& roots) {
  return library::DeleteInside(target, roots);
}

}  // namespace

void RegisterRunnerRoutes(httplib::Server& http, Services& s) {
  // --- runners --------------------------------------------------------------

  http.Get("/v1/runners", [&s](const Request&, Response& res) {
    const runner::RunnerRegistry registry(s.config);
    json out = json::array();
    for (const model::RunnerBuild& build : registry.DiscoverAll()) {
      json entry = model::ToJson(build);
      entry["label"] = runner::BuildLabel(build.kind, build.release);
      if (build.kind == "proton" || build.kind == "wine") {
        const std::filesystem::path dir = runner::BuildDir(build);
        entry["removable"] = paths::IsWithin(dir, runner::RunnerRoots(s.config, build.kind));
        const auto family = runner::FamilyOfBuild(s.config, build.kind, build.release, dir.filename().string());
        entry["source"] = family ? family->id : "";
      }
      out.push_back(std::move(entry));
    }
    SendJson(res, std::move(out));
  });

  http.Get("/v1/runners/sources", [&s](const Request& req, Response& res) {
    const std::string kind = Param(req, "kind");
    json out = json::array();
    for (const runner::RunnerFamily& family : runner::Families(s.config, kind)) {
      out.push_back({{"id", family.id}, {"kind", family.kind}, {"label", family.label}});
    }
    SendJson(res, std::move(out));
  });

  http.Get("/v1/runners/catalog", [&s](const Request& req, Response& res) {
    const std::string kind = Param(req, "kind", "proton");
    auto family = runner::FamilyFor(s.config, kind, Param(req, "source"));
    if (!family) return SendError(res, 404, family.error());
    auto releases = runner::ListFamilyReleases(*family);
    if (!releases) return SendError(res, 502, releases.error());
    const runner::RunnerRegistry registry(s.config);
    const std::vector<model::RunnerBuild> installed = runner::BuildsOfKind(registry, kind);
    json out = json::array();
    for (const auto& r : *releases) {
      out.push_back({{"tag", r.tag}, {"name", runner::ReleaseName(kind, r)},
                     {"label", runner::BuildLabel(kind, runner::ReleaseName(kind, r))}, {"source", family->id},
                     {"asset_name", r.asset_name}, {"size_bytes", r.size_bytes},
                     {"published_at", r.published_at}, {"has_checksum", !r.checksum_url.empty()},
                     {"installed", runner::HasInstalled(installed, r)}});
    }
    SendJson(res, std::move(out));
  });

  http.Post("/v1/runners/download", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("kind") || !body.contains("tag")) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"kind": "proton"|"wine", "tag": "...", "source": "<optional source id>"})");
    }
    const std::string kind = body["kind"];
    const std::string tag = body["tag"];
    auto family = runner::FamilyFor(s.config, kind, body.value("source", std::string()));
    if (!family) return SendError(res, 404, family.error());

    auto releases = runner::ListFamilyReleases(*family);
    if (!releases) return SendError(res, 502, releases.error());
    const auto match = std::ranges::find(*releases, tag, &runner::ReleaseAsset::tag);
    if (match == releases->end()) {
      return SendError(res, 404, "release_not_found", std::format("no {} release tagged \"{}\"", family->label, tag));
    }
    s.InstallRunner(req, res, kind, family->id, *match, /*replacing=*/"");
  });

  // Only removable builds; the distro updates its own packages.
  http.Get("/v1/runners/updates", [&s](const Request&, Response& res) {
    const runner::RunnerRegistry registry(s.config);
    json out = json::array();
    for (const auto& update : runner::FindRunnerUpdates(s.config, registry)) {
      out.push_back({{"reference", update.build.Reference()}, {"source", update.family.id},
                     {"tag", update.latest.tag}, {"name", runner::ReleaseName(update.build.kind, update.latest)},
                     {"label", runner::BuildLabel(update.build.kind,
                                                  runner::ReleaseName(update.build.kind, update.latest))}});
    }
    SendJson(res, std::move(out));
  });

  http.Post("/v1/runners/update", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("reference") || !body["reference"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"reference": "kind:name"})");
    }
    const std::string reference = body["reference"];
    const runner::RunnerRegistry registry(s.config);
    for (const auto& update : runner::FindRunnerUpdates(s.config, registry)) {
      if (update.build.Reference() != reference) continue;
      return s.InstallRunner(req, res, update.build.kind, update.family.id, update.latest, reference);
    }
    SendError(res, 409, "no_update", std::format("no newer release for \"{}\"", reference));
  });

  http.Get("/v1/runners/tools", [](const Request&, Response& res) {
    const std::string umu = runner::UmuRunPath();
    const std::string winetricks = runner::WinetricksPath();
    SendJson(res, json::array({
                      {{"id", "umu"}, {"label", "umu-launcher"}, {"installed", !umu.empty()}, {"path", umu},
                       {"doc", "Runs Proton builds outside Steam. Without it no Proton build can be used."}},
                      {{"id", "winetricks"}, {"label", "winetricks"}, {"installed", !winetricks.empty()},
                       {"path", winetricks}, {"doc", "Installs runtimes and fixes into a game's prefix."}},
                  }));
  });

  http.Post(R"(/v1/runners/tools/(umu|winetricks)/setup)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    s.events.Publish(id + ".setup.started", json::object());
    s.StartJob(req, res, "setup", id, "Setting up " + id,
               [&s, id](JobRegistry::Progress&) -> Result<json> {
                 Result<void> installed;
                 if (id == "umu") {
                   const auto releases = runner::ListReleases(s.config, "umu");
                   if (!releases) {
                     installed = std::unexpected(releases.error());
                   } else if (releases->empty()) {
                     installed = Err("no_release_found", "no umu-launcher release found");
                   } else if (auto path = runner::InstallToolBinary(s.config, "umu", releases->front(), "umu-run"); !path) {
                     installed = std::unexpected(path.error());
                   }
                 } else {
                   installed = runner::InstallWinetricks();
                 }
                 if (!installed) {
                   log::Error("{} install failed: {}", id, installed.error().message);
                   s.events.Publish(id + ".setup.failed", FailedEvent(json::object(), installed.error()));
                   return std::unexpected(installed.error());
                 }
                 s.events.Publish(id + ".setup.finished", json::object());
                 return json{{"id", id}};
               },
               &s.operations);
  });

  http.Get(R"(/v1/runners/([^/]+)/schema)", [&s](const Request& req, Response& res) {
    const runner::RunnerRegistry registry(s.config);
    const runner::IRunner* found = registry.FindByKind(req.matches[1]);
    if (!found) return SendError(res, 404, "unknown_runner_kind", "no runner of that kind");
    SendJson(res, found->SettingsSchema());
  });

  // Only builds inside a search path can be removed, so the system wine can't be.
  http.Delete(R"(/v1/runners/([^:]+):(.+))", [&s](const Request& req, Response& res) {
    const std::string kind = req.matches[1];
    const std::string name = req.matches[2];
    if (name == "auto" || name == "latest") {
      return SendError(res, 400, "invalid_reference", "name a concrete build, not \"auto\"/\"latest\"");
    }

    const runner::RunnerRegistry registry(s.config);
    auto resolved = registry.Resolve(kind + ":" + name);
    if (!resolved) return SendError(res, 404, resolved.error());
    if (!resolved->build) {
      return SendError(res, 400, "not_a_build", std::format("\"{}\" has no separate installed builds", kind));
    }

    if (auto deleted = DeleteUnderRoot(runner::BuildDir(*resolved->build).string(), runner::RunnerRoots(s.config, kind)); !deleted) {
      return SendError(res, 400, deleted.error());
    }
    s.events.Publish("runners.removed", {{"kind", kind}, {"name", name}});
    SendJson(res, json::object());
  });
}

}  // namespace mira::api
