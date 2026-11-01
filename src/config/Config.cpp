#include "config/Config.h"

#include <algorithm>
#include <array>
#include <format>
#include <sstream>

#include "config/Schema.h"
#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/TomlJson.h"

namespace mira::config {
namespace {

constexpr mode_t kSettingsMode = 0600;  // it holds API keys
using nlohmann::json;

// Leads every settings.toml Mira writes. Older versions wrote every default, so a file without it
// is cleaned once on load.
constexpr std::string_view kHeader =
    "# Only the settings you change are kept here; the rest follow Mira's defaults.\n"
    "# `mira config describe` lists every setting with its default.\n\n";

void Erase(json& set, const json::json_pointer& pointer) {
  if (!pointer.empty() && set.contains(pointer))
    set[pointer.parent_pointer()].erase(pointer.back());
}

// Removes the tables left empty once their settings went, so the file doesn't list bare headers.
// An object-valued setting is a value, kept even when empty.
void Prune(json& table, const std::string& prefix) {
  for (auto it = table.begin(); it != table.end();) {
    const std::string key = prefix + it.key();
    if (it->is_object() && Schema::Instance().Find(key) == nullptr) {
      Prune(*it, key + ".");
      if (it->empty()) {
        it = table.erase(it);
        continue;
      }
    }
    ++it;
  }
}

// Drops the values an older Mira wrote out without anyone choosing them: today's default and the
// earlier ones. Whether anything was dropped.
bool DropWrittenDefaults(json& set) {
  bool dropped = false;
  for (const Entry& entry : Schema::Instance().Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (!set.contains(pointer)) continue;
    const json& value = set[pointer];
    if (value == entry.default_value || std::ranges::contains(entry.old_defaults, value)) {
      Erase(set, pointer);
      dropped = true;
    }
  }
  return dropped;
}

// Drops what no setting reads (settings since removed, the old [frontend] table), adding each
// dotted key to `dropped`. A setting's own value is its data, so it's never looked into.
void DropUnknown(json& table, const std::string& prefix, std::vector<std::string>& dropped) {
  for (auto it = table.begin(); it != table.end();) {
    const std::string key = prefix + it.key();
    if (Schema::Instance().Find(key) == nullptr) {
      if (it->is_object()) DropUnknown(*it, key + ".", dropped);
      if (!it->is_object() || it->empty()) {
        if (!it->is_object()) dropped.push_back(key);
        it = table.erase(it);
        continue;
      }
    }
    ++it;
  }
}

// A key with a dot above a setting ({"tags.folders": ...}) would be saved as one stray table key,
// since TOML quotes it: settings nest. Inside a setting's own value, keys are its data.
void FindDottedKeys(const json& table, const std::string& prefix,
                    std::vector<std::string>& problems) {
  for (const auto& [key, value] : table.items()) {
    const std::string dotted = prefix + key;
    if (key.find('.') != std::string::npos) {
      problems.push_back(std::format("{}: write it as nested tables, not a dotted key", dotted));
    } else if (value.is_object() && Schema::Instance().Find(dotted) == nullptr) {
      FindDottedKeys(value, dotted + ".", problems);
    }
  }
}

}  // namespace

Config::Config(std::filesystem::path file)
    : file_(file), frontend_file_(file.parent_path() / "frontend.toml") {
  document_ = Schema::Instance().Defaults();
}

void Config::ResetInvalid(json& whole,
                          const std::function<std::optional<json>(const Entry&)>& fallback,
                          std::string_view note) {
  const Schema& schema = Schema::Instance();
  for (const Entry& entry : schema.Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (!whole.contains(pointer)) continue;
    if (auto problem = schema.Validate(entry.key, whole[pointer])) {
      log::Warn("settings: {}: {} {}", entry.key, *problem, note);
      if (auto value = fallback(entry)) {
        whole[pointer] = std::move(*value);
      } else {
        Erase(whole, pointer);
      }
    }
  }
}

void Config::SetLocked(json set) {
  set_ = std::move(set);
  Prune(set_, "");
  document_ = Schema::Instance().Defaults();
  document_.merge_patch(set_);
}

std::string Config::TextLocked() const {
  return std::string(kHeader) + tomljson::ToTomlText(set_);
}

void Config::Load(const std::string& fallback) {
  std::lock_guard lock(mutex_);
  SetLocked(json::object());
  frontend_ = json::object();
  file_as_loaded_ = json::object();
  ++revision_;
  keep_file_ = false;
  keep_frontend_file_ = false;
  loaded_text_.clear();

  std::error_code ec;
  const auto write_text = [&](const auto& text) {
    std::ostringstream out;
    out << text;
    if (auto written =
            WriteFileAtomic(file_, out.str(), "config_write_failed", true, kSettingsMode);
        !written) {
      log::Warn("couldn't write {}: {}", file_.string(), written.error().message);
    }
  };
  const auto apply = [&](const toml::table& table, const std::string& text) {
    json whole = tomljson::ToJson(table);

    // An individual bad value falls back to its default rather than
    // rejecting the whole file, so one typo cannot leave the user with a
    // daemon that refuses to start.
    ResetInvalid(whole, [](const Entry&) { return std::nullopt; }, "(using the default)");
    loaded_text_ = text;
    file_as_loaded_ = whole;
    std::vector<std::string> unknown;
    DropUnknown(whole, "", unknown);
    const bool cleaned = !text.starts_with(kHeader) && DropWrittenDefaults(whole);
    if (cleaned || !unknown.empty()) {
      // Each kind of cleaning keeps its own copy, so a later one can't overwrite the first's.
      const char* suffix = cleaned ? ".with-defaults" : ".with-removed-settings";
      std::filesystem::copy_file(file_, std::filesystem::path(file_) += suffix,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (cleaned) {
        log::Info(
            "settings: kept only the values that differ from Mira's defaults (the file as it "
            "was is {}{})",
            file_.string(), suffix);
      }
      if (!unknown.empty()) {
        std::string keys;
        for (const std::string& key : unknown) keys += (keys.empty() ? "" : ", ") + key;
        log::Info(
            "settings: dropped what no setting reads any more: {} (the file as it was is {}{})",
            keys, file_.string(), suffix);
      }
      SetLocked(std::move(whole));
      loaded_text_ = TextLocked();
      write_text(loaded_text_);
      return;
    }
    SetLocked(std::move(whole));
  };

  if (!std::filesystem::exists(file_, ec)) {
    log::Info("no settings at {}, starting from the defaults", file_.string());
    loaded_text_ = TextLocked();
    write_text(loaded_text_);
  } else {
    // A file from before saves were owner-only.
    std::filesystem::permissions(
        file_, std::filesystem::perms::group_all | std::filesystem::perms::others_all,
        std::filesystem::perm_options::remove, ec);
    const auto read = files::ReadFile(file_);
    const std::string text = read.value_or("");
    toml::parse_result parsed = toml::parse(text, file_.string());
    if (!read && std::filesystem::exists(file_, ec)) {
      // Unreadable is not empty: defaults saved over it would lose every setting.
      keep_file_ = true;
      log::Error("could not read the settings at {}; continuing with defaults and not saving",
                 file_.string());
    } else if (parsed) {
      apply(parsed.table(), text);
    } else if (const auto broken = SetAside(file_)) {
      toml::parse_result last_good = toml::parse(fallback);
      if (!fallback.empty() && last_good) {
        log::Error(
            "settings at {} could not be parsed ({}); kept it as {} and went back to the last "
            "settings that loaded",
            file_.string(), parsed.error().description(), broken->string());
        write_text(fallback);
        apply(last_good.table(), fallback);
      } else {
        log::Error(
            "settings at {} could not be parsed ({}); kept it as {} and continuing with defaults",
            file_.string(), parsed.error().description(), broken->string());
        loaded_text_ = TextLocked();
        write_text(loaded_text_);
      }
    } else {
      keep_file_ = true;
      log::Error(
          "settings at {} could not be parsed ({}) or set aside ({}); continuing with defaults",
          file_.string(), parsed.error().description(), broken.error().message);
    }
  }

  // frontend.toml is opaque (no schema, nothing to validate) and entirely
  // optional: nothing has necessarily ever written to it yet.
  if (std::filesystem::exists(frontend_file_, ec)) {
    toml::parse_result parsed = toml::parse_file(frontend_file_.string());
    if (!parsed) {
      if (const auto broken = SetAside(frontend_file_)) {
        log::Error(
            "frontend settings at {} could not be parsed ({}); kept it as {} and continuing with "
            "none",
            frontend_file_.string(), parsed.error().description(), broken->string());
      } else {
        keep_frontend_file_ = true;
        log::Error(
            "frontend settings at {} could not be parsed ({}) or set aside ({}); continuing with "
            "none",
            frontend_file_.string(), parsed.error().description(), broken.error().message);
      }
    } else {
      frontend_ = tomljson::ToJson(parsed.table());
    }
  }

  if (read_ui_state_) {
    ui_state_ = read_ui_state_();
    if (!ui_state_.is_object()) ui_state_ = json::object();
  }
}

void Config::UseUiStateStore(std::function<json()> read, std::function<void(const json&)> write) {
  std::lock_guard lock(mutex_);
  read_ui_state_ = std::move(read);
  write_ui_state_ = std::move(write);
}

bool Config::TakeUiState(json& table) {
  if (!read_ui_state_ || !table.is_object()) return false;
  bool taken = false;
  for (const std::string_view key : kUiStateKeys) {
    const auto found = table.find(key);
    if (found == table.end()) continue;
    if (found->is_null()) {
      ui_state_.erase(std::string(key));
    } else {
      ui_state_[std::string(key)] = *found;
    }
    table.erase(found);
    taken = true;
  }
  return taken;
}

void Config::OnValidText(std::function<void(const std::string&)> callback) {
  std::lock_guard lock(mutex_);
  on_valid_text_ = std::move(callback);
}

Result<std::vector<std::string>> Config::SyncLocked() {
  const Schema& schema = Schema::Instance();
  const auto read = files::ReadFile(file_);
  if (!read) {
    std::error_code ec;
    if (std::filesystem::exists(file_, ec)) keep_file_ = true;  // unreadable, so not saved over
    return std::vector<std::string>{};
  }
  const std::string& text = *read;
  if (text == loaded_text_) {
    keep_file_ = false;  // readable again, as last loaded
    return std::vector<std::string>{};
  }

  toml::parse_result parsed = toml::parse(text, file_.string());
  if (!parsed) {
    const auto& error = parsed.error();
    return Err("settings_invalid",
               std::format("{} line {}: {}", file_.filename().string(), error.source().begin.line,
                           error.description()),
               "Fix the line, or change the setting in Mira. Until then the settings before the "
               "edit stay in use.");
  }
  json whole = tomljson::ToJson(parsed.table());
  ResetInvalid(
      whole,
      [this](const Entry& entry) -> std::optional<json> {
        const auto pointer = Schema::Pointer(entry.key);
        if (set_.contains(pointer)) return set_[pointer];
        return std::nullopt;
      },
      "(keeping the current value)");
  const json before = document_;
  SetLocked(std::move(whole));

  std::vector<std::string> changed;
  for (const Entry& entry : schema.Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (document_.value(pointer, json()) != before.value(pointer, json()))
      changed.push_back(entry.key);
  }
  loaded_text_ = text;
  keep_file_ = false;
  ++revision_;
  if (on_valid_text_) on_valid_text_(text);
  return changed;
}

Result<std::vector<std::string>> Config::Reload() {
  std::lock_guard lock(mutex_);
  return SyncLocked();
}

Result<void> Config::ChangeAndSave(const std::function<void()>& change) {
  std::lock_guard lock(mutex_);
  if (auto synced = SyncLocked(); !synced) {
    // The app's change is saved over a broken hand edit, which is kept as <file>.bad.
    if (const auto broken = SetAside(file_)) {
      log::Warn("{}; kept it as {} and saved the settings before it with this change",
                synced.error().message, broken->string());
    } else {
      keep_file_ = true;
    }
  }
  change();
  SetLocked(std::move(set_));
  ++revision_;  // every change to the settings saves
  if (keep_file_) return KeptFileError(file_);
  const std::string text = TextLocked();
  if (auto written = WriteFileAtomic(file_, text, "config_write_failed", true, kSettingsMode);
      !written)
    return written;
  loaded_text_ = text;
  if (on_valid_text_) on_valid_text_(text);
  return {};
}

Result<void> Config::SaveFrontendFile() {
  std::lock_guard lock(mutex_);  // frontend_ is also written by Patch and SetFrontendSettings
  if (keep_frontend_file_) return KeptFileError(frontend_file_);
  return WriteFileAtomic(frontend_file_, tomljson::ToTomlText(frontend_), "config_write_failed");
}

std::string Config::LoadedText() const {
  std::lock_guard lock(mutex_);
  return loaded_text_;
}

json Config::FileAsLoaded() const {
  std::lock_guard lock(mutex_);
  return file_as_loaded_;
}

json Config::Document() const {
  std::lock_guard lock(mutex_);
  return document_;
}

json Config::GetLocked(std::string_view key) const {
  const auto pointer = Schema::Pointer(key);
  if (document_.contains(pointer)) return document_[pointer];
  if (const Entry* entry = Schema::Instance().Find(key)) return entry->default_value;
  return json();
}

json Config::Get(std::string_view key) const {
  std::lock_guard lock(mutex_);
  return GetLocked(key);
}

Result<void> Config::Set(std::string_view key, const json& value) {
  if (auto problem = Schema::Instance().Validate(key, value)) {
    return Err("invalid_setting", std::format("{}: {}", key, *problem));
  }
  return ChangeAndSave([&] { set_[Schema::Pointer(key)] = value; });
}

Result<void> Config::Patch(const json& patch) {
  const Schema& schema = Schema::Instance();

  // Validate the whole patch first: a rejected patch must change nothing.
  std::vector<std::string> problems;
  for (const Entry& entry : schema.Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (!patch.contains(pointer) || patch[pointer].is_null()) continue;  // null: back to default
    if (auto problem = schema.Validate(entry.key, patch[pointer])) {
      problems.push_back(std::format("{}: {}", entry.key, *problem));
    }
  }
  // A key the schema doesn't know (a typo) would be saved unchecked. A setting's value may nest (a
  // list, a map). A removal (null) sets nothing; an empty object or list flattens to null too, so
  // look at the real value.
  if (patch.is_object()) {
    json settings = patch;
    settings.erase("frontend");
    FindDottedKeys(settings, "", problems);
  }
  const json flat = patch.flatten();  // named: items() would outlive a temporary
  for (const auto& item : flat.items()) {
    const std::string& pointer = item.key();
    if (pointer.empty() || patch[json::json_pointer(pointer)].is_null()) continue;
    if (pointer == "/frontend" || pointer.starts_with("/frontend/")) continue;
    std::string key = pointer.substr(1);
    std::ranges::replace(key, '/', '.');
    const std::string dotted = key;
    while (schema.Find(key) == nullptr && key.rfind('.') != std::string::npos)
      key.resize(key.rfind('.'));
    if (schema.Find(key) == nullptr) problems.push_back(std::format("{}: unknown setting", dotted));
  }
  if (!problems.empty()) {
    std::string joined;
    for (const std::string& problem : problems) {
      if (!joined.empty()) joined += "; ";
      joined += problem;
    }
    return Err("invalid_setting", joined);
  }

  json backend_patch = patch;
  if (backend_patch.contains("frontend") && !backend_patch["frontend"].is_object() &&
      !backend_patch["frontend"].is_null()) {
    return Err("invalid_setting", "frontend: expected an object");
  }
  if (backend_patch.contains("frontend")) {
    if (backend_patch["frontend"].is_object()) {
      json frontend_patch = backend_patch["frontend"];
      bool settings_changed = false;
      {
        std::lock_guard lock(mutex_);
        if (TakeUiState(frontend_patch)) write_ui_state_(ui_state_);
        settings_changed = !frontend_patch.empty();
        frontend_.merge_patch(frontend_patch);
      }
      if (settings_changed) {
        if (auto result = SaveFrontendFile(); !result) return result;
      }
    }
    backend_patch.erase("frontend");
  }
  // A null removes the value, back to the default.
  return ChangeAndSave([&] { set_.merge_patch(backend_patch); });
}

Result<void> Config::Reset(std::string_view key) {
  const Entry* entry = Schema::Instance().Find(key);
  if (entry == nullptr) return Err("unknown_setting", std::format("unknown setting \"{}\"", key));
  return ChangeAndSave([&] { Erase(set_, Schema::Pointer(key)); });
}

Result<void> Config::ResetAll() {
  return ChangeAndSave([&] { set_ = json::object(); });
}

json Config::FrontendSettings() const {
  std::lock_guard lock(mutex_);
  json merged = frontend_;
  merged.update(ui_state_);
  return merged;
}

void Config::SetFrontendSettings(json settings) {
  {
    std::lock_guard lock(mutex_);
    ui_state_ = json::object();
    TakeUiState(settings);
    if (write_ui_state_) write_ui_state_(ui_state_);
    frontend_ = std::move(settings);
  }
  if (auto result = SaveFrontendFile(); !result) {
    log::Error("failed to save frontend settings: {}", result.error().message);
  }
}

bool Config::GetBool(std::string_view key) const {
  const json value = Get(key);
  return value.is_boolean() && value.get<bool>();
}

std::int64_t Config::GetInt(std::string_view key) const {
  const json value = Get(key);
  return value.is_number() ? value.get<std::int64_t>() : 0;
}

double Config::GetDouble(std::string_view key) const {
  const json value = Get(key);
  return value.is_number() ? value.get<double>() : 0.0;
}

std::string Config::GetString(std::string_view key) const {
  const json value = Get(key);
  return value.is_string() ? value.get<std::string>() : std::string();
}

std::vector<std::string> Config::GetStringArray(std::string_view key) const {
  std::vector<std::string> out;
  const json value = Get(key);
  if (!value.is_array()) return out;
  for (const json& item : value) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

std::filesystem::path Config::GetPath(std::string_view key) const {
  return paths::Expand(GetString(key));
}

std::vector<std::filesystem::path> Config::GetPathArray(std::string_view key) const {
  std::vector<std::filesystem::path> out;
  for (const std::string& value : GetStringArray(key)) out.push_back(paths::Expand(value));
  return out;
}

}  // namespace mira::config
