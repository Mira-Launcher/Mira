#pragma once

#include <QString>
#include <string>
#include <vector>

namespace mira_gui::downloads {

// The links in pasted text, in order and without repeats.
std::vector<std::string> ParseLinks(const QString& text);

}  // namespace mira_gui::downloads
