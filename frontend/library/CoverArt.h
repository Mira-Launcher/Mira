#pragma once

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace mira_gui {

// Placeholder cover art, for a game with no fetched artwork: a grid of
// identical grey rectangles is unusable. Derived from the game's id, so it
// stays stable across restarts, renames, and every machine showing the
// same library.

QColor PlaceholderBase(const QString& seed);

// Up to two initials, from the first two words that start with a letter or
// digit. "The Witcher 3" reads better as "TW" than as "T3", so the numeric
// tail of a title is only used when there is nothing else.
QString CoverInitials(const QString& name);

QPixmap PlaceholderCover(const QString& name, const QString& seed, QSize size, qreal dpr);

}  // namespace mira_gui
