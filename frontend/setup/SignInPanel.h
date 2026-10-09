#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "SignInGuide.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class BrowserSketch;
class ProgressRail;
class SetupLog;

// Connects one source, step by step, beside a drawing of the page the user will see: a store
// (its helper tool fetched first, then a sign-in that's picked up from the clipboard or pasted),
// a game launcher (installed into its own prefix, then signed in inside it), or Steam's
// owned-games key.
class SignInPanel : public QWidget {
  Q_OBJECT

 public:
  // `id` is a store, a launcher, or "steam". Without `header` there's no Back, title or done
  // line, for a card that has its own. `stacked` puts the drawing under the steps, for a narrow
  // window. Mirad is first asked (and a store's helper tool fetched) once the panel shows.
  SignInPanel(const QString& id, QWidget* parent, bool header = true, bool stacked = false);

  // Asks mirad where things stand again.
  void Refresh();
  // Whether the source is connected, as last asked.
  bool Done() const { return done_; }
  // Shows "<what> <mirad's error>" under the steps.
  void ShowFailure(const QString& what, const ApiError& error);

 signals:
  void BackClicked();
  // Signed in, installed or a key saved. `just_now`: here, not found that way when first asked.
  void Connected(bool just_now);
  // A launcher's install started, or couldn't start.
  void InstallStarted();
  void InstallFailed();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  enum class Mode { Store, Launcher, SteamKey };

  void ShowSteps();
  void ShowBrowser(bool other);
  void ShowDone(bool done, const QString& account = {});
  void SetPreparing(bool preparing);
  void OpenPage();
  void CheckClipboard();
  void SignIn(const QString& text, bool from_clipboard);
  void SaveSteamKey(const QString& key);
  void Install();
  void HandleEvent(const std::string& type, const std::string& data);

  QString id_;
  Mode mode_ = Mode::Store;
  SignInGuide guide_;
  bool done_ = false;
  bool asked_ = false;     // Refresh has run
  bool answered_ = false;  // mirad has said where things stand
  bool opened_ = false;    // the page was opened, so the clipboard is watched
  bool installing_ = false;  // a launcher
  bool other_browser_ = false;
  double install_progress_ = -1;
  QString last_clipboard_;
  std::vector<SteamAccount> accounts_;

  QLabel* done_label_ = nullptr;  // in the header
  QWidget* preparing_ = nullptr;
  QWidget* body_ = nullptr;
  QWidget* browsers_ = nullptr;  // Firefox / Chrome or Edge, for guides that differ
  QVBoxLayout* steps_ = nullptr;
  QWidget* account_row_ = nullptr;
  QLabel* account_ = nullptr;
  QComboBox* account_choice_ = nullptr;
  BrowserSketch* sketch_ = nullptr;
  QLabel* caption_ = nullptr;
  QWidget* foot_ = nullptr;
  QLabel* watching_ = nullptr;
  QLineEdit* paste_ = nullptr;
  QPushButton* sign_in_ = nullptr;
  QLabel* error_ = nullptr;
  QPointer<ProgressRail> install_rail_;  // a launcher's, while it installs
  SetupLog* log_ = nullptr;
};

}  // namespace mira_gui
