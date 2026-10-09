#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "../activity/DownloadTracker.h"
#include "../window/LibraryWindow.h"
#include "Paint.h"
#include "../system/Distro.h"

class QProcess;
class QStackedWidget;

namespace mira_gui::bigscreen {

class Chrome;
struct QuickSettings;
class GameKeyboard;
class GamePage;
class GamepadInput;
class HeroBackground;
class Page;
class Notice;
class Sounds;

// Big screen mode: a fullscreen window for the couch, driven by a controller
// (through SDL3 when it's installed) or the keyboard. It reads the library
// LibraryWindow keeps current, and covers browsing, playing, installing and
// a few settings; everything else stays in the desktop window.
class BigScreenWindow : public QWidget {
  Q_OBJECT

public:
  explicit BigScreenWindow(LibraryServices services, QWidget* parent = nullptr);
  ~BigScreenWindow() override;

  const LibraryServices& services() const { return services_; }
  const FrontendPrefs& prefs() const { return prefs_; }
  // Saves just the big screen keys and applies them.
  void SetPrefs(const FrontendPrefs& prefs);
  double unit() const;
  // "xbox", "ps" or "nin": the setting, or the connected controller's.
  // "keys", "xbox", "ps" or "nin": which labels the hints show.
  QString GlyphKind() const;
  const GamepadInput& input() const { return *input_; }

  // The game or owned title with this key, fresh from the library.
  Item Find(const QString& key) const;
  // Whether big screen lists this game or store title: never hidden ones, apps only when set to.
  bool Browsable(const GameSummary& game) const;
  bool Browsable(const StoreTitle& title) const;
  Item ItemFor(const GameSummary& game) const;
  Item ItemFor(const StoreTitle& title) const;
  QPixmap Cover(const Item& item, QSize size) const;
  // The game's logo, when it has one and it's loaded (fetched along with its hero).
  QPixmap Logo(const Item& item) const;
  // The item's install in the downloads list, if there is one running or paused.
  const DownloadTracker::Entry* Download(const Item& item) const;
  // 0..1 while installing or paused, else -1.
  double Progress(const Item& item) const;

  void ShowHero(const Item& item);
  void OpenGame(const Item& item);
  void Toast(const QString& text);
  // Hints show keys (true) or the controller's buttons.
  void UseKeys(bool keys);
  // A yes/no question over the current page; `on_confirm` runs on yes.
  void Confirm(const QString& title, const QString& body, const QString& confirm_label,
               std::function<void()> on_confirm);

  // The actions every page shares.
  void Play(const Item& item);
  void Stop(const Item& item);
  void Install(const Item& item);
  void Pause(const Item& item);
  void Resume(const Item& item);
  void CancelInstall(const Item& item);
  void Uninstall(const Item& item);
  void TogglePin(const Item& item);
  // The action X does on a game: "Play", "Stop", "Install", or empty while installing.
  QString QuickActionLabel(const Item& item) const;
  void QuickAction(const Item& item);

  // A list of choices over the page; picking one runs it.
  void ShowMenu(const QString& title, std::vector<std::pair<QString, std::function<void()>>> entries);
  void Exit();
  void OpenSteamBigPicture();
  // "Suspend", "Reboot" or "PowerOff".
  void PowerAction(const char* action);
  // Upgrades the system's packages through pkexec, after asking.
  void UpdateSystem();
  // The update's latest output line; empty when none runs.
  QString UpdateStatus() const { return update_status_; }
  // Hitting the end of a list: a dull sound and a small knock.
  void Bump();

signals:
  void Closed();

protected:
  void closeEvent(QCloseEvent* event) override;
  bool event(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  friend class Chrome;
  // The Guide button's menu over a running game.
  struct Menu {
    QString title;
    std::vector<std::pair<QString, std::function<void()>>> entries;
    int focus = 0;
  };
  struct Dialog {
    QString title;
    QString body;
    QString confirm_label;
    std::function<void()> on_confirm;
    int focus = 1;  // 0 confirm, 1 cancel
  };

  enum Tab { kHomeTab, kCollectionsTab, kSearchTab, kDownloadsTab, kSettingsTab };
  void Navigate(Nav nav);
  void SelectTab(int index);
  void ShowPage(Page* page);
  void Back();
  void RaiseFromGame();
  void Guide();
  void ReturnToGame();
  void OpenGameKeyboard();
  void StartUpdate(const system::Distro& distro, const QStringList& command);
  void SetUpdateStatus(const QString& status);
  void Screenshot(const QString& game_name);
  void TogglePerformanceOverlay(const std::string& id);
  // The Guide menu's per-game settings: frame limit, overlay, GameMode.
  void OpenQuickSettings(const std::string& id, const QString& name);
  void ShowQuickSettings(const std::string& id, const QString& name, const QuickSettings& settings);
  // An app rather than a game is in front (launched from here), so the controller drives it with keys.
  bool AppInFront() const;
  void AppControl(Nav nav);
  void ApplyInputOptions();
  // Counts idle time from now, when a sleep time is set.
  void RestartIdle();
  // A message over the game when big screen isn't in front, else a toast.
  void Tell(const QString& title, const QString& detail = {});
  void FollowDownloads();
  // The game running now, if any.
  const GameSummary* RunningGame() const;
  void Feedback(Nav nav);
  // Ends the launch overlay once the game runs, and comes back when it exits.
  void FollowLaunch();

  LibraryServices services_;
  FrontendPrefs prefs_;
  GamepadInput* input_ = nullptr;
  HeroBackground* hero_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  Chrome* chrome_ = nullptr;
  QList<Page*> tabs_;
  QStringList tab_names_;
  GamePage* game_page_ = nullptr;
  int tab_ = 0;
  std::optional<Dialog> dialog_;
  std::optional<Menu> menu_;
  bool keys_ = false;  // the keyboard was used last
  QElapsedTimer escape_armed_;  // the first Esc on Home
  Sounds* sounds_ = nullptr;
  GameKeyboard* keyboard_ = nullptr;
  Notice* notice_ = nullptr;
  // Guide waits for its release, so Guide + A can take a screenshot instead.
  QTimer guide_hold_;
  bool guide_chord_ = false;
  QTimer idle_;
  // Installs running at the last look, to tell when one finishes.
  QSet<QString> installing_;
  unsigned blanking_cookie_ = 0;
  QString toast_;
  QTimer toast_timer_;
  QTimer clock_timer_;
  // The game being started (shown over everything until it runs), and the one running from here.
  Item launching_;
  QString playing_;
  // A game was handed to Steam, so nothing says when it runs or ends: stay
  // behind until the player comes back.
  bool handed_off_ = false;
  QProcess* update_ = nullptr;
  QString update_status_;
  QString update_output_;
  QTimer launch_timeout_;
};

}  // namespace mira_gui::bigscreen
