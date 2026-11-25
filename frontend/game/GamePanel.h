#pragma once

#include <QWidget>
#include <vector>

#include "GameEditForm.h"

class QBoxLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class Button;
class CoverChip;
class LaidOutButton;
class SectionNav;

// The game card's left side: the cover, the name, its status and play time, Play, Store info and
// Change art, the saves and achievements lines, then a lower part that lists the cards and follows
// their scroll (the jump list), or holds the controls of the page open on the right with Back. The
// top stays put whichever page is open.
class GamePanel : public QWidget {
  Q_OBJECT

 public:
  explicit GamePanel(ArtworkStore* artwork, QWidget* parent = nullptr);

  CoverChip* Cover() const { return cover_; }
  QLabel* Title() const { return title_; }
  QLabel* Status() const { return status_; }
  QLabel* Played() const { return played_; }
  Button* Play() const { return play_; }
  // A button for each tool launch option, under Play.
  QHBoxLayout* Tools() const { return tools_; }
  QPushButton* DetailsButton() const { return details_; }
  QPushButton* ArtButton() const { return art_; }
  QPushButton* AchievementsButton() const { return achievements_; }
  // The saves line, split at its " · " into a left and a right part.
  void SetSaves(const QString& text, bool attention);
  void SetSavesShown(bool shown);
  // A title that isn't installed: no Play, no details, no saves or achievements lines.
  void SetTitleOnly();
  // Leaves the cover out, for a card too short for it.
  void SetCoverShown(bool shown);

  // The jump list: a row per card under its heading, jumping to it in `scroll` and following it.
  void SetJumps(const std::vector<GameEditForm::JumpGroup>& groups, QScrollArea* scroll);
  void ShowJumps();
  // A page's controls under Back and the page's title, in place of the jump list.
  void ShowPage(const QString& title, QWidget* controls);

 signals:
  void SavesClicked();
  void BackClicked();

 private:
  // Each row shown while its card is, with how many settings the card sets for the game.
  void SyncJumps();
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  // The saves line's two parts on one line, or the second under the first when they don't fit.
  void FitSavesLine();

  CoverChip* cover_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* played_ = nullptr;
  Button* play_ = nullptr;
  QHBoxLayout* tools_ = nullptr;
  QPushButton* details_ = nullptr;
  QPushButton* art_ = nullptr;
  QBoxLayout* buttons_ = nullptr;  // Store info and Change art
  LaidOutButton* saves_ = nullptr;
  QLabel* saves_left_ = nullptr;
  QLabel* saves_dot_ = nullptr;
  QLabel* saves_right_ = nullptr;
  QPushButton* achievements_ = nullptr;
  QWidget* facts_ = nullptr;  // the saves and achievements lines, their room kept

  QStackedWidget* lower_ = nullptr;
  QWidget* jumps_page_ = nullptr;
  QVBoxLayout* jumps_layout_ = nullptr;
  SectionNav* jump_nav_ = nullptr;
  struct JumpRow {
    QPushButton* button = nullptr;
    QLabel* note = nullptr;
    QWidget* card = nullptr;
  };
  std::vector<JumpRow> jump_rows_;
  std::vector<QLabel*> jump_headings_;
  std::vector<QWidget*> jump_cards_;  // what the rows were built for, to rebuild only on a change
  QWidget* page_ = nullptr;           // the page controls' holder
  QLabel* page_title_ = nullptr;
  QVBoxLayout* page_layout_ = nullptr;
  QWidget* page_controls_ = nullptr;  // the controls it holds now
};

}  // namespace mira_gui
