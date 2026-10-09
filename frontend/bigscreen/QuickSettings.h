#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <string>

namespace mira_gui::bigscreen {

// The per-game settings the Guide menu offers, applied the next time the game starts.
// MangoHud does both the overlay and the frame limit, through the game's environment.
struct QuickSettings {
  int fps_limit = 0;  // 0 for none
  bool overlay = false;
  bool gamemode = false;
};

void LoadQuickSettings(QObject* context, const std::string& id, std::function<void(bool ok, QuickSettings)> done);
void SaveQuickSettings(QObject* context, const std::string& id, const QuickSettings& settings,
                       std::function<void(QString error)> done);

}  // namespace mira_gui::bigscreen
