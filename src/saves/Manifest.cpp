#include "saves/Manifest.h"

#include <algorithm>
#include <chrono>
#include <fkYAML.hpp>
#include <fstream>
#include <set>
#include <sstream>

#include "core/Log.h"
#include "core/Strings.h"
#include "model/Types.h"
#include "runner/Exec.h"

namespace mira::saves {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// The lists row keeping where the index came from and when it was checked.
constexpr std::string_view kStateList = "saves_manifest";
// Raised whenever what ParseManifest keeps changes, so an existing index is built again.
constexpr int kIndexFormat = 1;
constexpr std::int64_t kCheckEvery = 24 * 60 * 60;
// After a failed check, how long before something needing the index tries again.
constexpr std::int64_t kRetryAfter = 60 * 60;

json NodeToJson(const fkyaml::node& node) {
  if (node.is_mapping()) {
    json object = json::object();
    for (const auto& [key, value] : node.map_items()) {
      if (key.is_string()) object[key.get_value<std::string>()] = NodeToJson(value);
    }
    return object;
  }
  if (node.is_sequence()) {
    json array = json::array();
    for (const fkyaml::node& item : node) array.push_back(NodeToJson(item));
    return array;
  }
  if (node.is_string()) return node.get_value<std::string>();
  if (node.is_boolean()) return node.get_value<bool>();
  if (node.is_integer()) return node.get_value<std::int64_t>();
  if (node.is_float_number()) return node.get_value<double>();
  return nullptr;
}

const fkyaml::node* Child(const fkyaml::node& node, const char* key) {
  if (!node.is_mapping() || !node.contains(key)) return nullptr;
  return &node[key];
}

// A store id as the manifest writes it: a number, or now and then a string.
std::string IdText(const fkyaml::node& node) {
  if (node.is_integer()) return std::to_string(node.get_value<std::int64_t>());
  if (node.is_string()) return node.get_value<std::string>();
  return {};
}

std::vector<std::pair<std::string, std::string>> Ids(const fkyaml::node& entry) {
  std::vector<std::pair<std::string, std::string>> ids;
  const auto add = [&](const char* kind, const fkyaml::node* node) {
    if (node == nullptr) return;
    if (node->is_sequence()) {
      for (const fkyaml::node& item : *node)
        if (std::string ref = IdText(item); !ref.empty()) ids.emplace_back(kind, std::move(ref));
    } else if (std::string ref = IdText(*node); !ref.empty()) {
      ids.emplace_back(kind, std::move(ref));
    }
  };
  if (const fkyaml::node* steam = Child(entry, "steam")) add("steam", Child(*steam, "id"));
  if (const fkyaml::node* gog = Child(entry, "gog")) add("gog", Child(*gog, "id"));
  if (const fkyaml::node* more = Child(entry, "id")) {
    add("steam", Child(*more, "steamExtra"));
    add("gog", Child(*more, "gogExtra"));
    add("lutris", Child(*more, "lutris"));
  }
  return ids;
}

std::string ReadFile(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The status and ETag of the last response in a curl -D file (a redirect writes one block per
// hop). Status 0 when there's none, as for file://.
std::pair<int, std::string> LastResponse(const fs::path& headers) {
  int status = 0;
  std::string etag;
  std::istringstream lines(ReadFile(headers));
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("HTTP/")) {
      const auto space = line.find(' ');
      status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
      etag.clear();
    } else if (strings::ToLower(line.substr(0, 5)) == "etag:") {
      etag = strings::Trim(line.substr(5));
    }
  }
  return {status, etag};
}

}  // namespace

std::string Normalize(std::string_view name) {
  std::string out;
  bool gap = false;
  for (std::size_t at = 0; at < name.size(); ++at) {
    const std::string_view rest = name.substr(at);
    if (rest.starts_with("™") || rest.starts_with("’")) {  // ™, ’
      at += 2;
      continue;
    }
    if (rest.starts_with("®") || rest.starts_with("©")) {  // ®, ©
      at += 1;
      continue;
    }
    const auto c = static_cast<unsigned char>(name[at]);
    if (c == '\'') continue;
    if (c < 0x80 && !std::isalnum(c)) {
      gap = true;
      continue;
    }
    if (gap && !out.empty()) out += ' ';
    gap = false;
    out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  }
  return out;
}

Result<std::vector<store::MetadataStore::SaveEntry>> ParseManifest(const std::string& yaml) {
  std::vector<store::MetadataStore::SaveEntry> entries;
  try {
    const fkyaml::node root = fkyaml::node::deserialize(yaml);
    if (!root.is_mapping())
      return Err("manifest_invalid", "the download isn't a list of save locations");
    entries.reserve(root.size());
    for (const auto& [key, value] : root.map_items()) {
      if (!key.is_string()) continue;
      store::MetadataStore::SaveEntry entry{.name = key.get_value<std::string>()};
      entry.norm = Normalize(entry.name);
      if (const fkyaml::node* alias = Child(value, "alias"); alias && alias->is_string()) {
        entry.alias_of = alias->get_value<std::string>();
        entries.push_back(std::move(entry));
        continue;
      }
      json kept = json::object();
      for (const char* part : {"files", "registry"}) {
        if (const fkyaml::node* node = Child(value, part);
            node && node->is_mapping() && node->size() > 0)
          kept[part] = NodeToJson(*node);
      }
      if (kept.empty()) continue;
      if (const fkyaml::node* cloud = Child(value, "cloud"); cloud && cloud->is_mapping())
        kept["cloud"] = NodeToJson(*cloud);
      entry.entry = kept.dump(-1, ' ', false, json::error_handler_t::replace);
      entry.ids = Ids(value);
      entries.push_back(std::move(entry));
    }
  } catch (const std::exception& e) {
    return Err("manifest_invalid",
               std::format("the save-location list doesn't parse: {}", e.what()));
  }

  // Aliases of entries that were left out lead nowhere.
  std::set<std::string> games;  // copies: erase_if moves the entries the names live in
  for (const auto& entry : entries)
    if (entry.alias_of.empty()) games.insert(entry.name);
  if (games.empty()) return Err("manifest_invalid", "the save-location list names no games");
  std::erase_if(entries, [&](const auto& entry) {
    return !entry.alias_of.empty() && !games.contains(entry.alias_of);
  });
  return entries;
}

json ManifestState(const config::Config& config, const store::MetadataStore& cache) {
  const json state = cache.ReadList(std::string(kStateList));
  const bool same = state.is_object() &&
                    state.value("url", "") == config.GetString("saves.manifest_url") &&
                    state.value("format", 0) == kIndexFormat;
  return {{"url", config.GetString("saves.manifest_url")},
          {"games", same ? cache.SaveEntryCount() : 0},
          {"fetched_at", same ? state.value("fetched_at", std::int64_t{0}) : 0},
          {"checked_at", same ? state.value("checked_at", std::int64_t{0}) : 0}};
}

bool ManifestDue(const config::Config& config, const store::MetadataStore& cache) {
  const json state = cache.ReadList(std::string(kStateList));
  const std::int64_t now = model::NowSeconds();
  if (state.is_object() && now - state.value("tried_at", std::int64_t{0}) < kRetryAfter &&
      state.value("checked_at", std::int64_t{0}) < state.value("tried_at", std::int64_t{0}))
    return false;
  const json current = ManifestState(config, cache);
  return current["games"] == 0 || now - current["checked_at"].get<std::int64_t>() >= kCheckEvery;
}

Result<json> UpdateManifest(const config::Config& config, store::MetadataStore& cache,
                            const fs::path& work_dir) {
  const std::string url = config.GetString("saves.manifest_url");
  json state = cache.ReadList(std::string(kStateList));
  if (!state.is_object()) state = json::object();
  const bool indexed = state.value("url", "") == url && state.value("format", 0) == kIndexFormat &&
                       cache.SaveEntryCount() > 0;
  state["tried_at"] = model::NowSeconds();
  [[maybe_unused]] auto tried = cache.WriteList(std::string(kStateList), state);

  std::error_code ec;
  fs::create_directories(work_dir, ec);
  const fs::path file = work_dir / "save-locations.yaml";
  const fs::path headers = work_dir / "save-locations.headers";
  const auto clean_up = [&] {
    fs::remove(file, ec);
    fs::remove(headers, ec);
  };

  Command command;
  command.argv = {"curl",       "-sSLf", "--compressed", "--connect-timeout", "20",
                  "--max-time", "600",   "-D",           headers.string(),    "-o",
                  file.string()};
  if (indexed && !state.value("etag", "").empty())
    command.argv.insert(command.argv.end(), {"-H", "If-None-Match: " + state.value("etag", "")});
  command.argv.insert(command.argv.end(), {"--url", strings::EscapeUrl(url)});
  const auto ran = runner::RunAndWait(command);
  if (!ran || ran->exit_code != 0) {
    clean_up();
    return Err("download_failed",
               std::format("couldn't download the save-location list from {}: {}", url,
                           !ran ? ran.error().message
                                : std::format("curl exited {}: {}", ran->exit_code, ran->output)),
               kConnectionHint);
  }
  const auto [status, etag] = LastResponse(headers);
  const auto unchanged = [&](std::string why) -> Result<json> {
    clean_up();
    state["checked_at"] = model::NowSeconds();
    if (!etag.empty()) state["etag"] = etag;
    if (auto written = cache.WriteList(std::string(kStateList), state); !written)
      return std::unexpected(written.error());
    log::Info("the save-location list is unchanged ({})", why);
    return json{{"changed", false}, {"games", cache.SaveEntryCount()}};
  };
  if (indexed && status == 304) return unchanged("the server says so");

  std::string text = ReadFile(file);
  clean_up();
  const std::string hash = strings::StableHash(text);
  if (indexed && state.value("hash", "") == hash) return unchanged("same contents");

  const auto started = std::chrono::steady_clock::now();
  auto entries = ParseManifest(text);
  text = std::string();
  if (!entries) return std::unexpected(entries.error());
  if (auto replaced = cache.ReplaceSaveEntries(*entries); !replaced)
    return std::unexpected(replaced.error());
  log::Info("indexed {} save-location entries in {} ms", entries->size(),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  started)
                .count());
  entries->clear();
  entries->shrink_to_fit();

  const std::int64_t now = model::NowSeconds();
  state = {{"url", url},        {"format", kIndexFormat}, {"etag", etag},   {"hash", hash},
           {"fetched_at", now}, {"checked_at", now},      {"tried_at", now}};
  if (auto written = cache.WriteList(std::string(kStateList), state); !written)
    return std::unexpected(written.error());
  return json{{"changed", true}, {"games", cache.SaveEntryCount()}};
}

}  // namespace mira::saves
