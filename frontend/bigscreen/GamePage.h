#pragma once

#include <QPixmap>

#include <optional>
#include <vector>

#include "../client/Types.h"
#include "Page.h"

class QNetworkAccessManager;

namespace mira_gui::bigscreen {

// One game: its cover, status and play time, with Play, Install, Pause,
// Pin and Uninstall as its state allows.
class GamePage : public Page {
  Q_OBJECT

public:
  explicit GamePage(BigScreenWindow* window);

  // `from` is where Back returns to.
  void Open(const Item& item, Page* from);
  Page* from() const { return from_; }

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  enum class Action { Play, Stop, Install, Pause, Resume, Cancel, Pin, Uninstall };
  struct Button {
    Action action;
    QString label;
  };
  std::vector<Button> Buttons() const;
  void Refresh();
  void LoadDetails(const QString& key);
  void FetchShots(const QString& key, const std::vector<std::string>& urls);
  void LoadOwnShots();

  Item item_;
  Page* from_ = nullptr;
  int focus_ = 0;
  QNetworkAccessManager* network_ = nullptr;
  QString description_;
  QString proton_tier_;  // lowercase, e.g. "gold"
  std::string controller_;  // "full", "partial" or empty
  bool metadata_loaded_ = false;
  std::optional<GameSession> last_session_;
  std::vector<QPixmap> shots_ = std::vector<QPixmap>(3);
  std::vector<QPixmap> own_shots_;  // the user's screenshots, newest first
};

}  // namespace mira_gui::bigscreen
