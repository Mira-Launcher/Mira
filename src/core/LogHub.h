#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mira::loghub {

// Live logs by channel name ("daemon", "setup:office", "install:epic:Fortnite",
// "runner:proton:GE-Proton10-4"). Each keeps its last lines in memory; a
// reader asks for what came after the cursor it was given last time.
struct Page {
  std::vector<std::string> lines;
  std::uint64_t next = 0;  // the cursor to ask for next
  bool active = false;     // something is still writing to it
  // The line being redrawn in place (a progress bar, written with '\r'), or the text still waiting
  // for its newline. Not in `lines`: it changes until it ends, and then becomes one line there.
  std::string live;
};

// Starts the channel over: its old lines go, and it counts as active.
void Begin(std::string_view channel);
// Where channels are kept on disk. With a directory set, every channel Begin starts is also appended to
// <dir>/<channel>.log, line by line and flushed, so a crash or a restart does not take the log with it, and Read
// serves that file for a channel mirad has not written since it started. "daemon" and "game:" channels are
// left out: the daemon has its own log and a game's output is written to its file by mira-run.
void SetJournalDirectory(const std::filesystem::path& dir);
// Finished writing; the lines stay for reading.
void End(std::string_view channel);
// Text as it comes. A newline ends a line; a '\r' starts the line over, so a progress bar redrawn many times is one line that keeps changing (Page::live), not one line per redraw.
void Append(std::string_view channel, std::string_view text);
// With no `after`, the last `tail` lines.
Page Read(std::string_view channel, const std::uint64_t* after, int tail);

}  // namespace mira::loghub
