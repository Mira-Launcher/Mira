#pragma once

#include <QRect>
#include <QWidget>
#include <string>

#include "../client/Types.h"

class QLabel;

namespace mira_gui {

// A floating, non-modal preview shown after a short dwell over a game: name,
// status, ProtonDB tier, platform, last played, playtime, developer/genres.
// Install path and description are left out: this is a glance; the per-game
// edit page and GameDetailsCard have them. A store title also shows its
// Steam reviews, since it has no detail page.
class HoverCard : public QWidget {
  Q_OBJECT

 public:
  explicit HoverCard(QWidget* parent = nullptr);

  // Fills in what's already known (no round trip), then fetches the rest
  // (developer/genres/ProtonDB tier) with one GET /v1/games/{id}/metadata.
  // Safe to call again for a different game while still visible -- the
  // fetch result is dropped if a newer ShowGame has since moved on.
  // `hint` is a muted last line, e.g. what a click does.
  void ShowGame(const GameSummary& game, bool running, const QString& hint = QString());
  // A store title that isn't in the library yet, with its cached details
  // (GET /v1/library/metadata) once they arrive. Having no genres, it lists those of its Steam
  // tags that are among `library_tags`. `size` is a muted line under the status, when known.
  void ShowTitle(const QString& source, const QString& ref, const QString& title,
                 const QString& status, const QString& size, const QString& detail,
                 std::vector<std::string> library_tags);
  // Shows the card beside `anchor` (global), and keeps it there as it grows.
  void PopUpBeside(const QRect& anchor);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  void ShowMetadata(const GameMetadata& metadata, const std::vector<std::string>& genres);
  void Reset();
  void Reposition();

  std::string game_id_;
  QRect anchor_;
  QLabel* name_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* size_line_ = nullptr;
  QLabel* protondb_ = nullptr;
  QLabel* platform_line_ = nullptr;
  QLabel* played_line_ = nullptr;
  QLabel* developer_line_ = nullptr;
  QLabel* review_line_ = nullptr;
  QLabel* error_line_ = nullptr;
  QLabel* hint_line_ = nullptr;
};

}  // namespace mira_gui
