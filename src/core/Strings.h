#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mira::strings {

std::string ToLower(std::string_view input);
std::string Trim(std::string_view input);
std::vector<std::string> Split(std::string_view input, char delimiter);

// Splits one command line like a shell: on spaces, with 'single' and "double" quotes keeping an argument
// together (a backslash escapes inside double quotes). How a game's `args` string reads.
std::vector<std::string> SplitArgs(std::string_view text);
// The inverse: quotes what needs it, so SplitArgs gives `args` back.
std::string JoinArgs(const std::vector<std::string>& args);

// Shell-style glob with '*' and '?', matched against the whole string.
bool GlobMatch(std::string_view pattern, std::string_view text);

// Filesystem-safe identifier derived from a game name, used for prefix
// directory names.
std::string Slugify(std::string_view input);

// Strips the noise that repacks and stores leave on directory names, e.g.
// "Celeste-v1.4.0.0-AnkerGames" -> "Celeste".
std::string CleanGameName(std::string_view directory_name);

// 0..1 similarity, used to score an executable name against its folder name.
double Similarity(std::string_view a, std::string_view b);

}  // namespace mira::strings
