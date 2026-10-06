#pragma once

#include <cstdint>
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
};

// Starts the channel over: its old lines go, and it counts as active.
void Begin(std::string_view channel);
// Finished writing; the lines stay for reading.
void End(std::string_view channel);
// Text as it comes: split on newlines (and '\r', which progress bars redraw with); a partial line waits for the rest.
void Append(std::string_view channel, std::string_view text);
// With no `after`, the last `tail` lines.
Page Read(std::string_view channel, const std::uint64_t* after, int tail);

}  // namespace mira::loghub
