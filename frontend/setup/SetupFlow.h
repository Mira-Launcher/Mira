#pragma once

#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>

namespace mira_gui::setup {

// The stores Set up Mira offers, in the order it lists them and walks through their sign-ins.
inline const QStringList kStores = {"steam", "epic", "gog", "amazon", "itch", "humble"};

// What has been answered so far, which decides the pages that follow.
struct Choices {
  bool games = false;
  bool apps = false;
  QStringList stores = {"steam", "epic", "gog"};
  bool office = true;  // Microsoft 365, picked on the applications page
  QStringList office_apps = {"word", "excel", "powerpoint"};
  QStringList found;  // "steam", "lutris": switched on on the page of what's found here
  // A skipped page's picks bring no pages of their own.
  bool stores_skipped = false;
  bool apps_skipped = false;
  bool skipped_all = false;  // Skip setup on the first page
};

// The pages in order: "welcome", "use", "found", "stores", "apps", "sign:<store>", "office",
// "look", "sidebar", "done".
QStringList Flow(const Choices& choices);

// The sources the picks turn on; every other one is turned off once setup is done, unless setup
// was skipped as a whole.
QStringList SourcesOn(const Choices& choices);

enum class Screen { Desktop, SteamDeck, Tv };

// A Steam Deck by its 1280x800 panel, a TV by a diagonal over 40 inches. `millimetres` is the
// screen's physical size, empty when the display doesn't say.
Screen ScreenKind(QSize pixels, QSizeF millimetres);

}  // namespace mira_gui::setup
