#pragma once

#include <optional>
#include <string>
#include <vector>

#include "HeroBackdrop.h"

class QHBoxLayout;

class QLabel;
class QPushButton;
class QStackedWidget;

namespace mira_gui {

class Button;

class AchievementsPage;
class ArtPickerPanel;
class SavesPage;
class StoreInfoPage;
class ChangeBar;
class GameEditForm;
class GamePanel;
class GameLibraryModel;

// A game's settings card over its art: the panel on the left (cover, name, Play, the saves and
// achievements lines, then the jump list or an open page's controls), the settings' cards in
// columns on the right (or the art picker, Saves, Achievements or the store info in their place)
// with a change bar. Built fresh for each open, so it starts synced to what's saved rather than to
// a discarded earlier edit.
class GameCard : public CardBackdrop {
  Q_OBJECT

 public:
  GameCard(const std::string& id, GameLibraryModel* library, ArtworkStore* artwork,
           QWidget* parent = nullptr);
  // A store title that isn't installed, by the id it will have: only its cover, in the art picker.
  // `title` carries its id, name and source.
  GameCard(const GameSummary& title, GameLibraryModel* library, ArtworkStore* artwork,
           QWidget* parent = nullptr);

  const std::string& id() const { return id_; }
  bool IsDirty() const;
  // Saves the form's edits; Saved follows on success.
  void Save();
  // Back and Esc: out of the art picker or an open page, then RequestClose.
  void Back();
  // The Saves page, once the game's saves have loaded.
  void OpenSaves();
  // Asks first when the edits aren't saved; CloseRequested once it may close.
  void RequestClose();
  // A new cover for `id`, which may be another game's.
  void UpdateCover(const std::string& id);
  // The panel's width, the cards' column width, and whether the panel shows the cover.
  void SetLayout(int panel_width, int column_width, bool cover);

 signals:
  void CloseRequested();
  void Saved();
  void PlayClicked();
  // A tag chip was clicked: show the library's games with that tag.
  void TagFilterRequested(QString tag);

 private:
  // Both constructors' common part; `title` is null for a game.
  void Build(const GameSummary* title);
  void resizeEvent(QResizeEvent* event) override;
  // Swaps the form for the art picker, and back.
  void OpenArtPicker();
  void CloseArtPicker(bool applied = false);
  bool ArtPickerOpen() const;
  // The form's change count, and room under its cards while the bar shows.
  void UpdateBar();
  // Play or Stop, as the game's state allows.
  QString Noun() const;  // "game", or "app"
  void UpdatePlay();
  // The name and status line, from the library's copy of the game.
  void UpdateIdentity();
  // Whether a live event carries a new record of this game.
  bool ChangesThisGame(const std::string& type, const std::string& data) const;
  // The cached achievements, then a refresh in the background when there are none yet or they're
  // over a day old (`refresh`).
  void LoadAchievements(bool refresh);
  void OpenAchievements();
  void CloseAchievements();
  bool AchievementsOpen() const;
  void OpenStoreInfo();
  void CloseStoreInfo();
  bool StoreInfoOpen() const;
  // GET /v1/games/{id}/saves into the saves line and the page, again on every change to them.
  void LoadSaves();
  void CloseSaves();
  bool SavesOpen() const;
  // A page in the settings' place, with its title and controls in the panel; ShowForm goes back.
  void ShowStackPage(QWidget* page, const QString& title, QWidget* controls);
  void ShowForm();

  std::string id_;
  // A not-installed title's card has no form, so form_ stays null.
  std::optional<GameSummary> title_info_;
  GameLibraryModel* library_ = nullptr;
  ArtworkStore* artwork_ = nullptr;
  GameEditForm* form_ = nullptr;
  CoverChip* cover_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* status_ = nullptr;
  // The form, and the art picker once first opened.
  QStackedWidget* stack_ = nullptr;
  ArtPickerPanel* picker_ = nullptr;
  // "48 of 52 achievements" under the saves line, which opens their page.
  QPushButton* achievements_button_ = nullptr;
  AchievementsPage* achievements_page_ = nullptr;
  StoreInfoPage* store_info_ = nullptr;
  int column_width_ = 0;  // SetLayout's, for a page made after it
  std::optional<AchievementsResult> achievements_;
  SavesPage* saves_page_ = nullptr;
  std::optional<SavesResult> saves_;
  int saves_request_ = 0;  // the newest LoadSaves: an older answer arriving after it is dropped
  bool open_saves_when_loaded_ = false;
  QPushButton* art_button_ = nullptr;
  Button* play_ = nullptr;
  QHBoxLayout* tools_ = nullptr;  // a button for each tool launch option, under Play
  GamePanel* panel_ = nullptr;
  Button* close_ = nullptr;
  std::vector<std::string> tool_ids_;
  // The form's unsaved changes, or the picker's pick while it's open.
  ChangeBar* bar_ = nullptr;
  // Room kept under the last card for the bar. It only grows: shrinking it as the bar hides would
  // pull a card scrolled to the bottom down.
  int bottom_room_ = 0;
  // Set by "Save and exit", so the save that follows closes the card.
  bool close_after_save_ = false;
  bool saving_ = false;
};

}  // namespace mira_gui
