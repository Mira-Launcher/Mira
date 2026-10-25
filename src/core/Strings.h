#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mira::strings {

std::string ToLower(std::string_view input);
// Each all-lowercase word capitalised, as Steam names its tags: "open world" is "Open World", but
// "free to play" is "Free to Play" and "RPG", "eSports" or "Co-op" are left as they are.
std::string TitleCase(std::string_view input);
std::string Trim(std::string_view input);
std::vector<std::string> Split(std::string_view input, char delimiter);

// Splits one command line like a shell: on spaces, with 'single' and "double" quotes keeping an
// argument together (a backslash escapes inside double quotes). How a game's `args` string reads.
std::vector<std::string> SplitArgs(std::string_view text);
// The inverse: quotes what needs it, so SplitArgs gives `args` back.
std::string JoinArgs(const std::vector<std::string>& args);
// `arg` in single quotes for sh, so nothing in it ($, `, \) is expanded.
std::string ShellQuote(std::string_view arg);
// `arg` as one quoted argument of a desktop entry's Exec= line, escaped as the spec asks.
std::string DesktopExecArg(std::string_view arg);

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

// A GitHub repository link, or one release's: ("owner/name", the tag or ""). Nothing for another
// link.
std::optional<std::pair<std::string, std::string>> ParseGithubUrl(std::string_view url);
// "owner/name" from a GitHub link, or `text` itself when it isn't one.
std::string GithubRepo(std::string_view text);

// `url` with the characters a URL can't hold (spaces, quotes, non-ASCII) percent-encoded, so curl
// sends it as written. Stores hand out such URLs as-is (Epic's art names have spaces); an existing
// %xx is kept.
std::string EscapeUrl(std::string_view url);

// 16 hex digits of FNV-1a: stable across builds and runs, unlike std::hash, for names kept on disk.
std::string StableHash(std::string_view text);

}  // namespace mira::strings
