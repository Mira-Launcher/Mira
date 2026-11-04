#pragma once

#include <QString>
#include <string>
#include <vector>

namespace mira_gui::downloads {

// The links in pasted text, in order and without repeats.
std::vector<std::string> ParseLinks(const QString& text);

// A game's name from its first file: "Some_Game.part01.rar" is "Some Game".
QString GuessName(const QString& file);

}  // namespace mira_gui::downloads
