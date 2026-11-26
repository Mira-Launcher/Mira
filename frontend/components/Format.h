#pragma once

#include <QString>
#include <QStringList>
#include <string>
#include <vector>

// How the GUI writes counts, sizes and paths, so every label says them alike.
namespace mira_gui {

// "a, b and c".
QString JoinAnd(QStringList items);
// "a, b, c", as a store lists developers or genres.
QString JoinNames(const std::vector<std::string>& names);

// "1 game", "3 games": `count` and `item`, plural unless it's one (`plural`, else `item` + "s").
QString Counted(qsizetype count, const QString& item, const QString& plural = {});
// "43 of 45 games", "2 of 2 apps", "45 of 47 games and apps": `shown` out of a set of `games`
// games and `apps` apps, named for what's in it.
QString ShownOf(qsizetype shown, qsizetype games, qsizetype apps);

// A size as every label shows one: "1.8 GB", "12.5 MB".
QString SizeText(qint64 bytes);

// A path as labels show it: "~/Mira/Games" for one in the home folder.
QString HomeRelative(const QString& path);

// A "~" or "~/..." path with the home folder written out; any other path as it is.
QString ExpandHome(const QString& path);

}  // namespace mira_gui
