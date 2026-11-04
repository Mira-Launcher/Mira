#pragma once

#include <QObject>
#include <QString>
#include <string>

class QWidget;

namespace mira_gui {

class DownloadTracker;
class GameLibraryModel;

// Updates Mira: asks before closing anything, downloads, installs, then closes the games, stops
// mirad and starts the new build. Games close last, so a dismissed password or a failed download
// costs nothing.
class MiraUpdater : public QObject {
  Q_OBJECT

 public:
  enum class State { Idle, Downloading, Installing, Closing, Restarting, Failed };

  MiraUpdater(GameLibraryModel* library, DownloadTracker* downloads, QWidget* window);

  State Current() const { return state_; }
  bool Busy() const { return state_ != State::Idle && state_ != State::Failed; }
  int Percent() const { return percent_; }             // while downloading
  const QString& Message() const { return message_; }  // "30 MB of 72 MB"
  const QString& Error() const { return error_; }      // when Failed

  // The whole update, for a release `latest`. Does nothing if the user declines.
  void Start(const QString& latest);
  // While downloading.
  void Cancel();

 signals:
  void Changed();   // another state
  void Progress();  // more downloaded

 private:
  void SetState(State state, const QString& error = {});
  void Install(const std::vector<std::string>& command);
  void CloseGames();
  void WaitForGames(int attempts_left);
  void Restart();
  void WaitForDaemon(int attempts_left);

  GameLibraryModel* library_;
  DownloadTracker* downloads_;
  QWidget* window_;
  State state_ = State::Idle;
  int percent_ = 0;
  QString message_;
  QString error_;
  std::string job_;  // the running download's job
};

}  // namespace mira_gui
