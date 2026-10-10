#include "SetupFlow.h"

#include <cmath>

namespace mira_gui::setup {

QStringList Flow(const Choices& choices) {
  QStringList flow = {"welcome", "use"};
  if (choices.games) flow << "found" << "stores";
  if (choices.apps) flow << "apps";
  if (choices.games && !choices.stores_skipped) {
    for (const QString& store : kStores) {
      if (choices.stores.contains(store)) flow << "sign:" + store;
    }
  }
  if (choices.apps && choices.office && !choices.apps_skipped) flow << "office";
  flow << "look" << "sidebar" << "done";
  return flow;
}

QStringList SourcesOn(const Choices& choices) {
  QStringList on = {"local"};
  if (choices.games) {
    for (const QString& id : choices.found) on << id;
    if (!choices.stores_skipped) {
      for (const QString& store : choices.stores) {
        if (!on.contains(store)) on << store;
      }
    }
  }
  if (choices.apps && choices.office && !choices.apps_skipped) on << "office";
  return on;
}

Screen ScreenKind(QSize pixels, QSizeF millimetres) {
  if (pixels == QSize(1280, 800) || pixels == QSize(800, 1280)) return Screen::SteamDeck;
  const double inches =
      std::hypot(millimetres.width(), millimetres.height()) / 25.4;
  return inches > 40 ? Screen::Tv : Screen::Desktop;
}

}  // namespace mira_gui::setup
