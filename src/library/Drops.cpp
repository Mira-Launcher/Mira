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

std::string NameOf(const fs::path& path) {
  std::error_code ec;
  return strings::CleanGameName(
      (fs::is_regular_file(path, ec) ? path.stem() : path.filename()).string());
}

// An installer split in parts (setup.exe beside setup-1.bin, setup-2.bin): the setup and its parts,
// setup first; empty when `path` is none of one. Each part drops on its own, but they only work
// together.
std::vector<fs::path> InstallerParts(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return {};
  const std::string stem = path.stem().string();
  fs::path setup;
  if (strings::ToLower(path.extension().string()) == ".exe") {
    setup = path;
  } else if (const std::size_t dash = stem.rfind('-'); dash != std::string::npos) {
    // Its setup, whatever the case of its name ("GAME.EXE" beside "GAME-1.bin").
    const std::string wanted = strings::ToLower(stem.substr(0, dash) + ".exe");
    for (const fs::directory_entry& entry : fs::directory_iterator(path.parent_path(), ec)) {
      if (strings::ToLower(entry.path().filename().string()) == wanted && entry.is_regular_file(ec))
        setup = entry.path();
    }
    if (setup.empty()) return {};
  } else {
    return {};
  }
  const std::string prefix = setup.stem().string() + "-";
  std::vector<fs::path> parts{setup};
  for (const fs::directory_entry& entry : fs::directory_iterator(path.parent_path(), ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file(ec) && name.starts_with(prefix) &&
        strings::ToLower(entry.path().extension().string()) == ".bin")
      parts.push_back(entry.path());
  }
  if (parts.size() == 1) return {};
  return parts;
}

}  // namespace

// A Windows, Linux or HTML game, or a folder holding one, rather than a ROM.
bool IsProgram(const config::Config& config, const fs::path& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    return !Detector(SettingsFromConfig(config)).Detect(path).candidates.empty();
  }
  if (IsWebPage(path)) return LooksLikeWebGame(path);
  const std::string extension = strings::ToLower(path.extension().string());
  return extension == ".exe" || extension == ".msi" || extension == ".appimage" ||
         extension == ".sh";
}

bool IsMiraFoldersOwn(const config::Config& config, const fs::path& path) {
  const std::string name = path.filename().string();
  if (name.starts_with('.')) return true;
  if (std::ranges::contains(std::vector<std::string>{"games", "applications", "prefixes", "roms",
                                                     "saves", "archives"},
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

void Drops::Sort(const fs::path& dropped, bool ask) {
  std::error_code ec;
  if (!fs::exists(dropped, ec)) return Forget(dropped);
  fs::path path = dropped;
  bool gathered = false;
  if (const std::vector<fs::path> parts = InstallerParts(dropped); !parts.empty()) {
    // Gathered in a folder of their own and sorted as one game, its .bin parts never as ROMs.
    if (dropped != parts.front()) return;
    const fs::path folder = dropped.parent_path() / NameOf(dropped);
    if (!fs::exists(folder, ec) && fs::create_directory(folder, ec)) {
      // All or none: a part that won't move puts back the ones that did.
      std::vector<fs::path> moved;
      for (const fs::path& part : parts) {
        fs::rename(part, folder / part.filename(), ec);
        if (ec) break;
        moved.push_back(part);
      }
      if (ec) {
        std::error_code undo;
        for (const fs::path& part : moved) fs::rename(folder / part.filename(), part, undo);
        fs::remove(folder, undo);
      }
    } else if (!ec) {
      ec = std::make_error_code(std::errc::file_exists);
    }
    if (ec) {
      log::Warn("couldn't gather {}'s parts into {}: {}", dropped.string(), folder.string(),
                ec.message());
    } else {
      path = folder;
      gathered = true;
    }
  }
  std::vector<std::string> emulators;
  for (const emulators::Emulator& emulator : emulators::All()) {
    // Not one that would take the folder around it, the Mira folder itself.
    if (!gathered && games_.Source(emulator.id).added && emulators::Takes(emulator, path) &&
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
