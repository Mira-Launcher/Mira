#pragma once

#include <string>

#include "HeroBackdrop.h"

class QLabel;
class QPushButton;
class QStackedWidget;

namespace mira_gui {

class ArtPickerPanel;
class ChangeBar;
class GameEditForm;
class GameLibraryModel;

// A game's settings card: its hero art across the top, Back and Change art,
// the name, status, cover and Play, then the edit form (or the art picker in
// its place) with a change bar. Built fresh for each open, so it starts
// synced to what's saved rather than to a discarded earlier edit.
class GameCard : public HeroBackdrop {
  Q_OBJECT

 public:
  GameCard(const std::string& id, GameLibraryModel* library, ArtworkStore* artwork,
           QWidget* parent = nullptr);

  const std::string& id() const { return id_; }
  bool IsDirty() const;
  // Saves the form's edits; Saved follows on success.
  void Save();
  // Back and Esc: out of the art picker, then Advanced, then RequestClose.
  void Back();
  // Asks first when the edits aren't saved; CloseRequested once it may close.
  void RequestClose();
  // A new cover for `id`, which may be another game's.
  void UpdateCover(const std::string& id);

 signals:
  void CloseRequested();
  void Saved();
  void PlayClicked();

 private:
  // Swaps the form for the art picker, and back.
  void OpenArtPicker();
  void CloseArtPicker(bool applied = false);
  bool ArtPickerOpen() const;
  // The form's change count, and room under its cards while the bar shows.
  void UpdateBar();
  // Play or Stop, as the game's state allows.
  void UpdatePlay();
  // The name and status line, from the library's copy of the game.
  void UpdateIdentity();
  // Whether a live event carries a new record of this game.
  bool ChangesThisGame(const std::string& type, const std::string& data) const;

  std::string id_;
  GameLibraryModel* library_ = nullptr;
  ArtworkStore* artwork_ = nullptr;
  GameEditForm* form_ = nullptr;
  CoverChip* cover_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* status_ = nullptr;
  // The form, and the art picker once first opened.
  QStackedWidget* stack_ = nullptr;
  ArtPickerPanel* picker_ = nullptr;
  QPushButton* art_button_ = nullptr;
  QPushButton* play_ = nullptr;
  // The form's unsaved changes, or the picker's pick while it's open.
  ChangeBar* bar_ = nullptr;
  // Set by "Save and exit", so the save that follows closes the card.
  bool close_after_save_ = false;
};

}  // namespace mira_gui
