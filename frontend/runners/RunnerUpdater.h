#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <string>

#include "../client/ApiError.h"

class QWidget;

namespace mira_gui {

class DownloadTracker;

// Updates a runner build. When the new one finishes, games have moved to it, so this offers to
// remove the old one, wherever the update was started and whichever page is showing.
class RunnerUpdater : public QObject {
  Q_OBJECT

 public:
  struct Build {
    std::string reference;
    std::string kind;
    std::string name;
    std::string label;
  };

  RunnerUpdater(DownloadTracker* downloads, QWidget* window);

  // Starts updating `old` to the release `name`. `started` says whether mirad took it.
  void Update(const Build& old, const std::string& release,
              std::function<void(bool ok, const ApiError& error)> started);

 private:
  void DownloadChanged(const QString& key);

  DownloadTracker* downloads_;
  QWidget* window_;
  std::map<std::string, Build> replacing_;  // new release name -> build it updates
};

}  // namespace mira_gui
