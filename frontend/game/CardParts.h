#pragma once

#include <QString>
#include <string>

class QLabel;

namespace mira_gui {

class ArtworkStore;
class SheetCard;
struct GameSummary;

// The game's cover before the title, then the title (SheetCard::SetHeading). Returns the title.
QLabel* AddGameCardHeading(SheetCard* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork);

// An installer format's name ("Inno Setup"), or empty for "unknown".
QString InstallerFormatName(const std::string& format);

}  // namespace mira_gui
