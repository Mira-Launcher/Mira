#include "library/Drops.h"

#include <algorithm>
#include <format>

#include "config/Schema.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "library/Detector.h"
#include "library/FolderTags.h"
#include "library/Import.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// A Windows or Linux program, or a folder holding one, rather than a ROM.
bool IsProgram(const config::Config& config, const fs::path& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    return !Detector(SettingsFromConfig(config)).Detect(path).candidates.empty();
  }
  const std::string extension = strings::ToLower(path.extension().string());
  return extension == ".exe" || extension == ".msi" || extension == ".appimage" ||
         extension == ".sh";
}

std::string NameOf(const fs::path& path) {
  std::error_code ec;
  return strings::CleanGameName(
      (fs::is_regular_file(path, ec) ? path.stem() : path.filename()).string());
}

}  // namespace

bool IsMiraFoldersOwn(const config::Config& config, const fs::path& path) {
  const std::string name = path.filename().string();
  if (name.starts_with('.')) return true;
  if (std::ranges::contains(
          std::vector<std::string>{"games", "applications", "prefixes", "roms", "saves"},
          strings::ToLower(name))) {
    return true;
  }
  for (const config::Entry& entry : config::Schema::Instance().Entries()) {
    if (entry.path != config::PathKind::Folder || entry.scope != config::Scope::Global ||
        entry.key == "mira_folders") {
      continue;
    }
    const std::vector<fs::path> folders = entry.type == config::Type::StringArray
                                              ? config.GetPathArray(entry.key)
                                              : std::vector{config.GetPath(entry.key)};
    for (const fs::path& folder : folders) {
      if (!folder.empty() && IsAtOrUnder(folder, path)) return true;
    }
  }
  return false;
}

void Drops::Sort(const fs::path& path, bool ask) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return Forget(path);
  std::vector<std::string> emulators;
  for (const emulators::Emulator& emulator : emulators::All()) {
    // Not one that would take the folder around it, the Mira folder itself.
    if (games_.Source(emulator.id).added && emulators::Takes(emulator, path) &&
        emulators::AddedSource(emulator, path) == path) {
      emulators.push_back(emulator.id);
    }
  }
  Drop drop{.path = path.lexically_normal(), .name = NameOf(path), .reason = {}, .choices = {}};
  if (IsProgram(config_, path)) {
    const ImportGuess guess = ClassifyImport(path);
    drop.name = guess.name;
    drop.reason = guess.reason;
    drop.choices = {"game", "app"};
    if (ask) {
      drop.reason = "It was already here when Mira started";
    } else if (guess.kind != ImportKind::kUnknown && emulators.empty()) {
      auto moved = MoveTo(path, std::string(ImportKindName(guess.kind)));
      if (moved) return;
      // Asked instead, so it doesn't sit there unseen.
      drop.reason = moved.error().message;
    }
  } else if (emulators.size() == 1 && ask) {
    drop.reason = "It was already here when Mira started";
  } else if (emulators.size() == 1) {
    auto moved = MoveTo(path, emulators.front());
    if (moved) return;
    drop.reason = moved.error().message;
  } else if (emulators.empty()) {
    log::Info("{} isn't a game, app or ROM Mira knows, so it stays where it is", path.string());
    return;
  } else {
    drop.reason = "More than one emulator runs it";
  }
  drop.choices.insert(drop.choices.end(), emulators.begin(), emulators.end());
  Ask(std::move(drop));
}

Result<void> Drops::MoveTo(const fs::path& path, const std::string& choice) {
  if (const std::optional<ImportKind> kind = ParseImportKind(choice)) {
    auto moved = ImportInto(config_, path, *kind);
    if (!moved) return std::unexpected(moved.error());
    log::Info("sorted {} into {}", path.string(), moved->parent_path().string());
    return {};
  }
  const emulators::Emulator* emulator = emulators::Find(choice);
  if (emulator == nullptr) return Err("invalid_choice", std::format("no emulator {}", choice));
  auto added = emulators::AddFiles(config_, games_, events_, *emulator, {path}, /*move=*/true);
  if (!added) return std::unexpected(added.error());
  log::Info("sorted {} into {}'s games", path.string(), emulator->name);
  return {};
}

void Drops::Ask(Drop drop) {
  {
    const std::lock_guard lock(mutex_);
    const auto known = std::ranges::find(drops_, drop.path, &Drop::path);
    // Sorted again (the watcher started over): asked once.
    if (known != drops_.end() && known->choices == drop.choices) return;
    if (known != drops_.end()) drops_.erase(known);
    drops_.push_back(drop);
  }
  log::Info("{} could go more than one place, so Mira asks", drop.path.string());
  events_.Publish("library.drop_unclear", ToJson(drop));
}

Result<void> Drops::Settle(const fs::path& path, const std::string& choice) {
  {
    const std::lock_guard lock(mutex_);
    const auto drop = std::ranges::find(drops_, path.lexically_normal(), &Drop::path);
    if (drop == drops_.end())
      return Err("drop_not_found", "nothing dropped there waits to be sorted");
    if (!std::ranges::contains(drop->choices, choice))
      return Err("invalid_choice", std::format("{} can't go to {}", drop->name, choice));
  }
  if (auto moved = MoveTo(path, choice); !moved) return moved;
  Forget(path);
  return {};
}

void Drops::Forget(const fs::path& path) {
  bool known = false;
  {
    const std::lock_guard lock(mutex_);
    known = std::erase_if(drops_, [&](const Drop& d) { return d.path == path.lexically_normal(); });
  }
  if (known) events_.Publish("library.drop_settled", {{"path", path.string()}});
}

std::vector<Drop> Drops::All() const {
  const std::lock_guard lock(mutex_);
  return drops_;
}

nlohmann::json Drops::ToJson(const Drop& drop) {
  nlohmann::json choices = nlohmann::json::array();
  for (const std::string& choice : drop.choices) {
    const emulators::Emulator* emulator = emulators::Find(choice);
    const std::string label = choice == "game"  ? "Games"
                              : choice == "app" ? "Applications"
                              : emulator        ? emulator->name
                                                : choice;
    choices.push_back({{"id", choice}, {"label", label}});
  }
  return {{"path", drop.path.string()},
          {"name", drop.name},
          {"reason", drop.reason},
          {"choices", std::move(choices)}};
}

}  // namespace mira::library
