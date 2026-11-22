#pragma once

#include <QString>
#include <functional>
#include <string>

class QHBoxLayout;
class QLabel;
class QToolButton;

namespace mira_gui {

class ArtworkStore;
class Card;
struct GameSummary;

// The pieces the in-window cards share (the installer, remove and add cards and the rest).

// A heading title, with a muted `subtitle` under it unless that's empty, and a close button with
// `close_tip`, which runs `close`. Returns the title.
QLabel* AddCardHeading(Card* card, const QString& title, const QString& close_tip,
                       const std::function<void()>& close, const QString& subtitle = {});
// The game's cover before the title, then AddCardHeading.
QLabel* AddGameCardHeading(Card* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork, const QString& close_tip,
                           const std::function<void()>& close);
// A back arrow before the title, for a page a card opened from its first one.
QToolButton* AddCardBack(Card* card, const std::function<void()>& back);
// A red line under the rows, hidden until there's an error to show.
QLabel* AddErrorLine(Card* card);
// The card's last row: buttons on the right, after a stretch.
QHBoxLayout* AddButtonRow(Card* card);

// An installer format's name ("Inno Setup"), or empty for "unknown".
QString InstallerFormatName(const std::string& format);

}  // namespace mira_gui
