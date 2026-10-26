#pragma once

#include <filesystem>
#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "store/MetadataStore.h"

namespace mira::saves {

// A game's name as the index compares it: lower case, without ™ ® © or apostrophes, other ASCII
// punctuation read as spaces, runs of spaces as one. "Baldur's Gate: Enhanced" and
// "baldurs-gate-enhanced" are both "baldurs gate enhanced".
std::string Normalize(std::string_view name);

// Ludusavi's manifest (github.com/mtkennerly/ludusavi-manifest) as the rows MetadataStore indexes,
// keeping each entry's `files` and `registry`. Entries listing neither are left out, and so are
// aliases of them, so a lookup moves on to the next way of matching. Err manifest_invalid when the
// text isn't the manifest.
Result<std::vector<store::MetadataStore::SaveEntry>> ParseManifest(const std::string& yaml);

// GET /v1/saves/manifest's answer: {url, games, fetched_at, checked_at}, the times 0 before the
// first download.
nlohmann::json ManifestState(const config::Config& config, const store::MetadataStore& cache);

// Whether the index is missing, came from another saves.manifest_url or was last checked over a
// day ago. After a failed check, not for an hour.
bool ManifestDue(const config::Config& config, const store::MetadataStore& cache);

// Downloads the manifest from saves.manifest_url into `work_dir` and indexes it, unless the server
// says it's unchanged (its ETag) or the download is identical to the one indexed. The download is
// deleted afterwards. {changed, games}.
Result<nlohmann::json> UpdateManifest(const config::Config& config, store::MetadataStore& cache,
                                      const std::filesystem::path& work_dir);

}  // namespace mira::saves
