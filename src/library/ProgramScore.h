#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include "model/Types.h"

namespace mira::library {

// What's in a game's folder, as far as it was looked at: lowercase paths in it, with "/" between.
using FolderContents = std::set<std::string>;

// How alike a program's name and a game's are, 0 to 1. Knows what a program's name adds
// ("-Win64-Shipping", "_x64") or shortens ("ac2" for Assassin's Creed II).
double ProgramNameMatch(std::string_view stem, std::string_view game);

// Whether the files around a program are those an engine keeps around a game's: Unity, Unreal,
// Godot, GameMaker, RPG Maker, Ren'Py, Electron and others.
bool HasEngineFiles(const std::filesystem::path& rel_path, const FolderContents& contents);

// How unlikely a program is to be the game, by what its name and folders say it's for (a config
// tool, a crash handler, an editor; "tools/"), 0 to 4. A word that is in the game's own name
// ("Server Simulator") says nothing.
double HelperPenalty(const std::filesystem::path& rel_path, std::string_view game);

// Folders below the game's that a game may well start from: an engine's "Binaries/Win64", "bin"
// or "game", or one named as the game is, aren't counted.
int CountedDepth(const std::filesystem::path& rel_path, std::string_view game);

// A native binary ("Celeste.bin.x86_64") beside the script that starts the right one.
bool HasStarter(const std::filesystem::path& rel_path, const FolderContents& contents);

// Built for 32-bit, by its name or folder.
bool Is32Bit(const std::filesystem::path& rel_path);

// 0 for a stub of 64 KB or less, up to 1 at 64 MB; 0 when the size isn't known.
double SizeScore(std::int64_t bytes);

// A Windows program that runs in a console window, as tools do and games don't.
bool IsConsoleProgram(const std::filesystem::path& exe);

}  // namespace mira::library
