#pragma once

#include <QSize>
#include <QString>
#include <string>

class QLabel;
class QWidget;

namespace mira_gui {

class ArtworkStore;
class SheetCard;
struct GameSummary;

// The game's cover at `size`, kept current as its art changes.
QLabel* MakeCover(QWidget* parent, const GameSummary& game, QSize size, ArtworkStore* artwork);

// The game's cover before the title, then the title (SheetCard::SetHeading). Returns the title.
QLabel* AddGameCardHeading(SheetCard* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork);

// An installer format's name ("Inno Setup"), or empty for "unknown".
QString InstallerFormatName(const std::string& format);

}  // namespace mira_gui
