#pragma once

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "../activity/DownloadTracker.h"
#include "../window/LibraryWindow.h"
#include "Paint.h"

class QStackedWidget;

namespace mira_gui::bigscreen {

class Chrome;
class GamePage;
class GamepadInput;
class HeroBackground;
class Page;

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
  QString GlyphKind() const;
  const GamepadInput& input() const { return *input_; }

  // The game or owned title with this key, fresh from the library.
  Item Find(const QString& key) const;
  Item ItemFor(const GameSummary& game) const;
  Item ItemFor(const StoreTitle& title) const;
  QPixmap Cover(const Item& item, QSize size) const;
  // The item's install in the downloads list, if there is one running or paused.
  const DownloadTracker::Entry* Download(const Item& item) const;
  // 0..1 while installing or paused, else -1.
  double Progress(const Item& item) const;

  void ShowHero(const Item& item);
  void OpenGame(const Item& item);
  void Toast(const QString& text);
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

  void Exit();

signals:
  void Closed();

protected:
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  friend class Chrome;
  struct Dialog {
    QString title;
    QString body;
    QString confirm_label;
    std::function<void()> on_confirm;
    int focus = 1;  // 0 confirm, 1 cancel
  };

  void Navigate(Nav nav);
  void SelectTab(int index);
  void ShowPage(Page* page);
  void Back();
  void RaiseFromGame();
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
  QString toast_;
  QTimer toast_timer_;
  QTimer clock_timer_;
  mutable QHash<QString, QPixmap> shown_covers_;
  // The game being started (shown over everything until it runs), and the one running from here.
  Item launching_;
  QString playing_;
  QTimer launch_timeout_;
};

}  // namespace mira_gui::bigscreen
