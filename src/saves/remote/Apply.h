#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "saves/Saves.h"
#include "saves/remote/Revisions.h"

// Replacing a game's saves with a downloaded revision, so that a crash halfway is finished later.
namespace mira::saves::remote {

// The survey's files, each once, as a revision's without checksums.
std::vector<SaveFile> LocalFiles(const Survey& survey);
// Tells whether the local files changed: their places, sizes and times. "none" when there are none.
std::string LocalMarker(const std::vector<SaveFile>& files);

// Where each of `files` goes on this computer, in order. Errs saves_unplaceable when the game has
// no such folder here, and remote_unsafe for a path outside its save locations or through a
// symlink inside one.
Result<std::vector<std::filesystem::path>> Targets(const Roots& roots, const Survey& survey,
                                                   const std::vector<RevisionFile>& files);

// Where a game's revision waits to be applied: inside its backups, so it survives a crash.
std::filesystem::path StagingFolder(const config::Config& config, const model::Game& game);

// Puts the revision downloaded to StagingFolder in place and removes local saves it lacks, once
// `before` (a snapshot) agreed. From its first write until it's done a journal stands, for
// FinishApply.
Result<void> Apply(const config::Config& config, const model::Game& game, const Survey& survey,
                   const Revision& revision, const std::function<Result<void>()>& before);

// Finishes an Apply that was cut short; nothing when there's none. Returns its revision's id.
Result<std::string> FinishApply(const config::Config& config, const model::Game& game,
                                const Survey& survey, const std::function<Result<void>()>& before);

}  // namespace mira::saves::remote
