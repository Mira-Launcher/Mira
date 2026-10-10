#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "../client/Types.h"
#include "../theme/Icons.h"
#include "../window/LibraryWindow.h"
#include "SetupFlow.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace mira_gui {

class DemoArt;
class SetupWork;
class StepDots;

// What every page of Set up Mira shares.
struct SetupContext {
  LibraryServices services;
  SetupWork* work = nullptr;
  setup::Choices* choices = nullptr;
  DemoArt* art = nullptr;  // the example games' covers and heroes, fetched for the previews
  FrontendPrefs* prefs = nullptr;  // as saved so far, applied whole so nothing else resets
  setup::Screen screen = setup::Screen::Desktop;
};

// One page of Set up Mira.
class SetupStep : public QWidget {
  Q_OBJECT

 public:
  explicit SetupStep(QWidget* parent = nullptr) : QWidget(parent) {}
  // Shown, after the pages before it may have changed what it shows.
  virtual void Enter() {}
  // Left with Next: saves what was picked and starts the work it needs.
  virtual void Leave() {}

 signals:
  // Done on its own, e.g. signed in from the clipboard: move on.
  void Finished();
  // What was picked changed, and with it maybe the pages that follow.
  void PicksChanged();
};

// The heading every page starts with: an icon on an accent tile, a small line over the title, and
// what the page is for. Returns the layout the page's content goes in.
QVBoxLayout* SetupPageLayout(SetupStep* page, icons::Glyph glyph, const QString& eyebrow,
                             const QString& title, const QString& lead);
// "Not now? ..." with a clock: where a step can be done later.
QWidget* SetupLaterLine(QWidget* parent, const QString& text);
// A raised group for a page's content.
QFrame* SetupCard(QWidget* parent);
// A setting in a setup card: its label, whose tooltip is `doc`, then `control` at the end of the
// line or, with `below`, under it. `after` goes right after the label.
QWidget* SetupRow(QWidget* parent, const QString& label, const QString& doc, QWidget* control,
                  bool below = false, QWidget* after = nullptr);
// Saves `change` to frontend.toml and applies the whole of what's saved, so the window follows.
void SaveSetupPrefs(const SetupContext& context, const FrontendPrefs& change);

// Set up Mira: a small window of pages with Back, Skip and Next, opened instead of the main window
// on first launch only. It introduces Mira, asks what it's for, then walks through only what was
// picked: what's found here, the stores and applications, signing in to each, then how Mira looks.
// Slow work starts a page ahead (SetupWork) and carries on after it closes.
class SetupWindow : public QWidget {
  Q_OBJECT

 public:
  // `prefs` is frontend.toml as read at start.
  SetupWindow(LibraryServices services, SetupWork* work, const FrontendPrefs& prefs,
              QWidget* parent = nullptr);

 signals:
  // Closed with Open Mira, Skip setup or the window's close. `big_screen`: open Mira in big screen.
  void Finished(bool big_screen);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  SetupStep* PageFor(const QString& key);
  void Go(int index);
  void Next();
  void Skip();
  void Finish();
  void ShowRunning();
  void FitHeight();

  setup::Choices choices_;
  FrontendPrefs prefs_;
  SetupContext context_;
  QStringList flow_;
  int current_ = 0;
  bool finished_ = false;
  bool fitted_ = false;

  QHash<QString, SetupStep*> pages_;
  QStackedWidget* stack_ = nullptr;
  QLabel* running_ = nullptr;
  QPushButton* back_ = nullptr;
  QPushButton* skip_ = nullptr;
  QPushButton* next_ = nullptr;
  StepDots* dots_ = nullptr;
};

}  // namespace mira_gui
