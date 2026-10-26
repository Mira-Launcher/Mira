#pragma once

#include <QDateTime>
#include <QString>
#include <cstdint>
#include <string>

#include "../client/Types.h"

// The words the game card's saves line and Saves page show, from GET /v1/games/{id}/saves.
namespace mira_gui::saves_text {

// "Today, 21:14", "Yesterday, 23:02", "20 Oct, 19:55", or with the year when it's another one.
// `in_sentence` lowercases "today" and "yesterday" for text after other words.
QString When(std::int64_t at, const QDateTime& now, bool in_sentence = false);

// Why a backup was taken: "After playing", "Before the first launch", ...
QString Reason(const std::string& reason);

// The card's line: "7 backups · last after today's session", or what stands in for backups.
QString Summary(const SavesResult& saves, const QDateTime& now);

// Whether the line reads as a warning: a failed backup or a cloud conflict.
bool NeedsAttention(const SavesResult& saves);

// A save folder as the page shows it: "prefix › AppData/LocalLow/Studio/Game" inside a Wine
// prefix's user folder, "~/.config/Game" in the home folder, else the path itself.
QString Path(const std::string& path, const QString& home);

// What the game's status means when there's nothing to list: empty for "kept" and "ready".
QString StatusNote(const SavesResult& saves);

// The cloud's state in a line: "In sync with GOG's cloud", "Changed here and in Epic Games's
// cloud", ...
QString CloudState(const CloudSaves& cloud);

}  // namespace mira_gui::saves_text
