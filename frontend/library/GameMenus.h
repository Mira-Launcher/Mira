#pragma once

#include <QObject>
#include <QPoint>
#include <QString>

#include <functional>
#include <string>
#include <vector>

#include "../client/Types.h"

class QMenu;
class QWidget;

namespace mira_gui {

// The right-click menus for one game and for several, and the tag changes
// they make. The grid, the sidebar, Continue playing and source pages share them.
class GameMenus : public QObject {
public:
  // What the menus need from the window that shows them.
  struct Host {
    std::function<const GameSummary*(const std::string& id)> find;
    // "Installing… 1.2 GB" while the game installs, else empty.
    std::function<QString(const std::string& id)> install_text;
    std::function<void(const std::string& id)> toggle_running;
    std::function<void(const std::string& id)> open_settings;
    std::function<void(const std::string& id)> open_details;
    std::function<void(const std::string& id)> offer_install;
    std::function<void(const std::string& id)> refresh_metadata;
    // The game was deleted from the library.
    std::function<void(const std::string& id)> removed;
    // Records a request already changed, applied without waiting for their events.
    std::function<void(const std::vector<GameSummary>& games)> upsert;
  };

  GameMenus(QWidget* parent, Host host);

  // `extra` adds entries after Play (e.g. a store's Update).
  void ShowGameMenu(const std::string& id, const QPoint& global_pos,
                    const std::function<void(QMenu&)>& extra = nullptr);
  // A reduced set of actions applied to every game in `ids` at once.
  void ShowBatchMenu(const std::vector<std::string>& ids, const QPoint& global_pos);
  // Adds (`present`) or removes `tag` on each game that doesn't already match, in one request.
  void SetTag(const std::vector<std::string>& ids, const std::string& tag, bool present);
  void ToggleTag(const std::string& id, const std::string& tag);

private:
  QWidget* parent_;
  Host host_;
};

}  // namespace mira_gui
