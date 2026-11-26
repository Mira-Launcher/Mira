#include "saves/remote/RemoteSync.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <utility>

#include "config/Resolver.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "saves/Manifest.h"
#include "saves/Saves.h"
#include "saves/remote/Apply.h"
#include "saves/remote/RemoteConfig.h"
#include "saves/remote/Revisions.h"

namespace mira::saves::remote {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// Copies of the local files in `folder`/files, with their checksums as copied.
Result<std::vector<RevisionFile>> Stage(const std::vector<SaveFile>& files,
                                        const fs::path& folder) {
  std::vector<RevisionFile> out;
  std::vector<fs::path> copies;
  for (const SaveFile& file : files) {
    const fs::path copy = folder / "files" / file.root / file.path;
    std::error_code ec;
    fs::create_directories(copy.parent_path(), ec);
    if (!ec) fs::copy_file(file.absolute, copy, ec);
    const auto bytes = ec ? 0 : fs::file_size(copy, ec);
    if (ec)
      return Err("saves_failed",
                 std::format("Couldn't copy {}: {}", file.absolute.string(), ec.message()));
    out.push_back({.root = file.root,
                   .path = file.path,
                   .bytes = bytes,
                   .mtime_ns = file.mtime_ns,
                   .sha256 = {}});
    copies.push_back(copy);
  }
  const auto sums = Sha256(copies);
  if (!sums) return std::unexpected(sums.error());
  for (std::size_t i = 0; i < out.size(); ++i) out[i].sha256 = (*sums)[i];
  return out;
}

// An empty folder for one sync, removed at its end unless an apply's journal stands in it.
Result<void> EmptyFolder(const fs::path& folder) {
  std::error_code ec;
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  if (ec) return Err("saves_failed", std::format("{}: {}", folder.string(), ec.message()));
  return {};
}

std::vector<RevisionFile> FilesOf(const std::string& text) {
  const json files = json::parse(text, nullptr, false);
  if (!files.is_array() || files.empty()) return {};
  const json doc = {{"format", 1}, {"id", "base"}, {"files", files}};
  const auto revision = ParseManifest(doc.dump(), "base");
  return revision ? revision->files : std::vector<RevisionFile>();
}

// The user's own name for the game's folder; empty for none.
std::string ChosenKey(const config::Config& config, const model::Game& game) {
  const std::string chosen =
      strings::Trim(config::Resolver(config, game.overrides).GetString("saves.remote_key"));
  return chosen.empty() ? chosen : strings::Slugify(chosen);
}

}  // namespace

std::string KeyFor(const model::Game& game, const std::string& entry_name) {
  // A folder's name on every remote, so short, plain and the same on every computer.
  const auto key = [](std::string_view kind, std::string_view text) {
    const auto plain = [](unsigned char c) { return c < 0x80; };
    const bool named = std::ranges::any_of(text, [](unsigned char c) { return std::isalnum(c); });
    if (text.empty() || (!named && std::ranges::all_of(text, plain))) return std::string();
    const std::string slug = strings::Slugify(text);
    // Slugs drop other scripts' letters, and are cut where too long.
    if (std::ranges::all_of(text, plain) && slug.size() <= 80)
      return std::format("{}-{}", kind, slug);
    return std::format("{}-{}-{}", kind, slug.substr(0, 60), strings::StableHash(text));
  };
  if (game.runner_ref.starts_with("steam:")) return key("steam", game.runner_ref.substr(6));
  for (const std::string_view store : {"gog", "epic", "amazon", "itch"})
    if (game.source == store && !game.source_ref.empty()) {
      // Ids may differ only in case, which slugs and some remotes don't keep.
      return std::format("{}-{}", key(store, game.source_ref),
                         strings::StableHash(game.source_ref));
    }
  if (!entry_name.empty()) return key("save", entry_name);
  return key("name", Normalize(game.name));
}

Result<json> RunRemote(const config::Config& config, const store::MetadataStore& cache,
                       store::GameStore& games, const model::Game& game, const Device& device,
                       CloudAction action, const std::function<Result<void>()>& before_download) {
  const auto info = FindRemote(config, "primary");
  if (!info) return Err("remote_none", "No remote is connected.");
  const Remote remote = RemoteFor(config, *info);

  Survey survey = SurveyFor(config, cache, game);
  store::RemoteSync row =
      games.RemoteSyncOf(game.id, info->slot).value_or(store::RemoteSync{.slot = info->slot});
  if (row.remote_id != info->id)
    row = {.slot = info->slot, .remote_id = info->id, .key = row.key, .pending = row.pending};
  if (const std::string chosen = ChosenKey(config, game); !chosen.empty() && chosen != row.key)
    row = {.slot = info->slot, .remote_id = info->id, .key = chosen, .pending = row.pending};
  if (row.key.empty())
    row.key = KeyFor(game, survey.match ? survey.match->entry.name : std::string());
  if (row.key.empty())
    return Err("remote_unsyncable", std::format("{} has no name to sync under.", game.name));
  for (const model::Game& other : games.All()) {
    if (other.id == game.id) continue;
    const auto theirs = games.RemoteSyncOf(other.id, info->slot);
    if (theirs && theirs->remote_id == info->id && theirs->key == row.key)
      return Err("remote_key_taken",
                 std::format("{} already syncs with that folder on {}.", other.name, info->name),
                 "Give this game its own folder name in its save settings.",
                 Fix::Game(game.id, "saves"));
  }

  const auto store_row = [&] {
    row.checked_at = model::NowSeconds();
    if (auto stored = games.SetRemoteSync(game.id, row); !stored)
      log::Warn("couldn't keep {}'s sync state: {}", game.id, stored.error().message);
  };
  const auto failed = [&](const Error& error) {
    row.error = error.code;
    store_row();
    return std::unexpected(error);
  };

  // A download cut short comes first: until it's finished the files here are neither side's.
  // One that never can be is given up, and what's here compared as it is.
  auto finished = FinishApply(config, game, survey, before_download);
  if (!finished &&
      (finished.error().code == "remote_unsafe" || finished.error().code == "saves_unplaceable" ||
       finished.error().code == "remote_damaged")) {
    DropApply(config, game);
    finished = std::string();
  }
  if (!finished) return failed(finished.error());
  if (!finished->empty()) survey = SurveyFor(config, cache, game);

  const auto listed = ListRevisions(config, remote, row.key);
  if (!listed) return failed(listed.error());
  const std::vector<Revision> leaves = Leaves(*listed);
  std::vector<SaveFile> local = LocalFiles(survey);
  const std::string marker = LocalMarker(local);

  const fs::path upload = StagingFolder(config, game).parent_path() / ".remote-upload";
  const struct Cleanup {
    const fs::path& folder;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(folder, ec);
    }
  } cleanup{upload};
  std::optional<std::vector<RevisionFile>> staged;
  const auto stage = [&]() -> Result<void> {
    if (staged) return {};
    std::uintmax_t total = 0;
    for (const SaveFile& file : local) total += file.bytes;
    if (total >
        static_cast<std::uintmax_t>(config::Resolver(config, game.overrides).GetInt("saves.max_mb"))
            << 20)
      return Err("saves_too_large",
                 std::format("{}'s saves are bigger than a backup may be.", game.name),
                 "Raise the size limit for this game's backups if all of it is saves.",
                 Fix::Game(game.id, "saves"));
    if (auto made = EmptyFolder(upload); !made) return made;
    auto files = Stage(local, upload);
    if (!files) return std::unexpected(files.error());
    staged = std::move(*files);
    return {};
  };

  if (!finished->empty()) {
    if (const auto applied = std::ranges::find(*listed, *finished, &Revision::id);
        applied != listed->end()) {
      row.base = applied->id;
      row.base_files = ManifestJson(*applied)["files"].dump();
      row.local_marker = marker;
    }
  }

  // Which sides changed since they were last in sync.
  const bool had_base = !row.base.empty();
  const bool single = leaves.size() == 1;
  bool local_changed = had_base ? marker != row.local_marker : !local.empty();
  bool remote_changed =
      had_base ? !leaves.empty() && (!single || leaves.front().id != row.base) : !leaves.empty();
  // All the saves gone is more likely a prefix made anew than every save deleted.
  const bool lost = had_base && local.empty() && row.base_files != "[]";
  if (local_changed && !local.empty()) {
    if (auto ready = stage(); !ready) return failed(ready.error());
    if (had_base && SameFiles(*staged, FilesOf(row.base_files))) {
      local_changed = false;
      row.local_marker = marker;
    }
  }
  std::string state(kSynced);
  bool same = false;
  if (leaves.empty()) {
    // Never synced, or the remote was emptied: what's here is all there is.
    state = local.empty() ? kSynced : kLocalNewer;
  } else if (lost || leaves.size() > 1 || (leaves.front().files.empty() && !local.empty())) {
    state = kConflict;
  } else if (local_changed && remote_changed) {
    same = staged && SameFiles(*staged, leaves.front().files);
    state = same ? kSynced : kConflict;
  } else if (remote_changed) {
    // A game found by name alone may not be the one the remote's saves are of.
    state = !had_base && row.key.starts_with("name-") ? kConflict : kCloudNewer;
  } else if (local_changed) {
    state = kLocalNewer;
  }

  json answer = json::object();
  if (!leaves.empty())
    answer["other"] = {{"device", leaves.front().device_name},
                       {"at", leaves.front().taken_at},
                       {"files", leaves.front().files.size()},
                       {"bytes", leaves.front().Bytes()}};
  row.other_device = leaves.empty() ? std::string() : leaves.front().device_name;
  row.other_at = leaves.empty() ? 0 : leaves.front().taken_at;
  const auto in_sync = [&](const Revision& revision, const std::string& now) {
    row.base = revision.id;
    row.base_files = ManifestJson(revision)["files"].dump();
    row.local_marker = now;
    row.synced_at = model::NowSeconds();
    row.pending = false;
    row.error.clear();
    state = kSynced;
  };
  if (same) in_sync(leaves.front(), marker);
  row.state = state;
  if (state == kSynced) {
    row.pending = false;
    row.error.clear();
  }
  store_row();
  if (action == CloudAction::Sync && state == kConflict)
    return Err("saves_conflict",
               std::format("{}'s saves changed both here and on {} since they were last in sync.",
                           game.name, info->name),
               "Choose which to keep. The other is backed up first, so either choice can be "
               "undone.",
               Fix::Game(game.id, "saves"));

  // Already the same: choosing a side changes nothing.
  const bool settled = state == kSynced && single;
  const bool download =
      !leaves.empty() && !settled &&
      (action == CloudAction::Download || (action == CloudAction::Sync && state == kCloudNewer));
  bool send =
      !local.empty() && !settled &&
      (action == CloudAction::Upload || (action == CloudAction::Sync && state == kLocalNewer));
  if (download) {
    const Revision& newest = leaves.front();
    if (newest.Bytes() >
        static_cast<std::uintmax_t>(config::Resolver(config, game.overrides).GetInt("saves.max_mb"))
            << 20)
      return failed(
          Error{.code = "saves_too_large",
                .message = std::format("{}'s saves on {} are bigger than a backup may be.",
                                       game.name, info->name),
                .hint = {}});
    const fs::path staging = StagingFolder(config, game);
    if (auto made = EmptyFolder(staging); !made) return failed(made.error());
    if (auto got = DownloadRevision(config, remote, row.key, newest, staging); !got)
      return failed(got.error());
    // Pending, so a crash halfway is picked up by the next retry.
    const bool owed = std::exchange(row.pending, true);
    row.state = kCloudNewer;
    store_row();
    if (auto applied = Apply(config, game, survey, newest, before_download); !applied) {
      row.pending = owed || ApplyUnfinished(config, game);
      row.state = state;
      return failed(applied.error());
    }
    survey = SurveyFor(config, cache, game);
    local = LocalFiles(survey);
    in_sync(newest, LocalMarker(local));
    answer["moved"] = "download";
    // Other leaves are joined to the one chosen, or they'd conflict again.
    send = leaves.size() > 1 && !local.empty();
    staged.reset();
  }
  if (send) {
    const std::string now = LocalMarker(local);
    row.pending = true;
    row.state = kLocalNewer;
    store_row();
    if (auto ready = stage(); !ready) return failed(ready.error());
    Revision revision{.id = NewRevisionId(model::NowSeconds()),
                      .device_id = device.id,
                      .device_name = device.name,
                      .game_name = game.name,
                      .taken_at = model::NowSeconds(),
                      .files = *staged};
    for (const Revision& leaf : leaves) revision.parents.push_back(leaf.id);
    if (auto sent = UploadRevision(config, remote, row.key, revision, upload); !sent)
      return failed(sent.error());
    in_sync(revision, now);
    if (!download) answer["moved"] = "upload";
    std::vector<Revision> all = *listed;
    all.push_back(revision);
    for (const std::string& id :
         Prunable(all, static_cast<std::size_t>(config.GetInt("saves.remote_keep"))))
      if (auto gone = DeleteRevision(config, remote, row.key, id); !gone) break;
  }
  row.state = state;
  store_row();
  answer["state"] = state;
  answer["revision"] = row.base;
  return answer;
}

Result<void> RunMirror(const config::Config& config, const store::MetadataStore& cache,
                       store::GameStore& games, const model::Game& game, const Device& device) {
  const auto primary = FindRemote(config, "primary");
  const auto mirror = FindRemote(config, "mirror");
  const auto from = games.RemoteSyncOf(game.id, "primary");
  if (!primary || !mirror || !from || from->remote_id != primary->id || from->base.empty())
    return {};
  store::RemoteSync row =
      games.RemoteSyncOf(game.id, "mirror").value_or(store::RemoteSync{.slot = "mirror"});
  if (row.remote_id != mirror->id || row.key != from->key)
    row = {.slot = "mirror", .remote_id = mirror->id, .key = from->key};
  if (row.base == from->base) return {};
  const auto store_row = [&] {
    row.checked_at = model::NowSeconds();
    if (auto stored = games.SetRemoteSync(game.id, row); !stored)
      log::Warn("couldn't keep {}'s mirror state: {}", game.id, stored.error().message);
  };
  const std::vector<SaveFile> local = LocalFiles(SurveyFor(config, cache, game));
  const auto settled = [&] {
    row.pending = false;
    row.error.clear();
    store_row();
  };
  // Changed or gone since: what's here isn't that revision. The next sync copies a newer one.
  if (local.empty() || LocalMarker(local) != from->local_marker) {
    settled();
    return {};
  }
  row.pending = true;
  store_row();
  const auto failed = [&](const Error& error) {
    row.error = error.code;
    store_row();
    return std::unexpected(error);
  };
  const fs::path staging = StagingFolder(config, game).parent_path() / ".remote-mirror";
  const struct Cleanup {
    const fs::path& folder;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(folder, ec);
    }
  } cleanup{staging};
  if (auto made = EmptyFolder(staging); !made) return failed(made.error());
  const auto staged = Stage(local, staging);
  if (!staged) return failed(staged.error());
  if (!SameFiles(*staged, FilesOf(from->base_files))) {
    settled();
    return {};
  }
  const Remote remote = RemoteFor(config, *mirror);
  const auto listed = ListRevisions(config, remote, row.key);
  if (!listed) return failed(listed.error());
  std::vector<Revision> all = *listed;
  if (std::ranges::find(all, from->base, &Revision::id) == all.end()) {
    Revision revision{.id = from->base,
                      .device_id = device.id,
                      .device_name = device.name,
                      .game_name = game.name,
                      .taken_at = model::NowSeconds(),
                      .files = *staged};
    for (const Revision& leaf : Leaves(all)) revision.parents.push_back(leaf.id);
    if (auto sent = UploadRevision(config, remote, row.key, revision, staging); !sent)
      return failed(sent.error());
    all.push_back(revision);
    for (const std::string& id :
         Prunable(all, static_cast<std::size_t>(config.GetInt("saves.remote_keep"))))
      if (auto gone = DeleteRevision(config, remote, row.key, id); !gone) break;
  }
  row.base = from->base;
  row.synced_at = model::NowSeconds();
  row.state = kSynced;
  settled();
  return {};
}

json RemoteOverview(const config::Config& config, const store::GameStore& games,
                    const model::Game& game, const Survey& survey) {
  const auto info = FindRemote(config, "primary");
  if (!info) return nullptr;
  json answer = {{"kind", "remote"},
                 {"store", info->name},
                 {"state", kUnknown},
                 {"synced_at", 0},
                 {"checked_at", 0},
                 {"local_at", 0},
                 {"cloud_at", 0},
                 {"files", 0},
                 {"bytes", 0},
                 {"device", ""},
                 {"locations", json::array()},
                 {"pending", false},
                 {"error", ""},
                 {"key", ""}};
  const std::vector<SaveFile> local = LocalFiles(survey);
  std::int64_t newest = 0;
  std::uintmax_t bytes = 0;
  for (const SaveFile& file : local) {
    newest = std::max(newest, file.mtime_ns / 1'000'000'000);
    bytes += file.bytes;
  }
  answer["local_at"] = newest;
  answer["files"] = local.size();
  answer["bytes"] = bytes;
  if (!config::Resolver(config, game.overrides).GetBool("saves.remote")) {
    answer["state"] = "off";
    return answer;
  }
  const auto row = games.RemoteSyncOf(game.id, info->slot);
  if (!row || row->remote_id != info->id) return answer;
  std::string state = row->state.empty() ? std::string(kUnknown) : row->state;
  // Played since the check: the local side has changed too.
  if (!row->base.empty() && LocalMarker(local) != row->local_marker) {
    if (state == kSynced) state = kLocalNewer;
    if (state == kCloudNewer) state = kConflict;
  }
  answer["state"] = state;
  answer["synced_at"] = row->synced_at;
  answer["checked_at"] = row->checked_at;
  answer["cloud_at"] = row->other_at;
  answer["device"] = row->other_device;
  answer["pending"] = row->pending;
  answer["error"] = row->error;
  answer["key"] = row->key;
  return answer;
}

}  // namespace mira::saves::remote
