#pragma once

#include <QWidget>
#include <string>

#include "../client/Types.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class FlatCombo;
class Switch;

// A game's launch options as rows with Edit and Remove, to go under their SettingRow from one edge
// of the card to the other, as ListEdit's. Its row shows EmptyLabel and AddButton; which one Play
// starts and whether that's the only choice offered go in rows of their own.
class LaunchOptionsEdit : public QWidget {
  Q_OBJECT

 public:
  explicit LaunchOptionsEdit(QWidget* parent = nullptr);

  // `windows`: its options can run in its prefix; `install_path` is where the file picker opens;
  // `store`: the store that starts the game signed in ("Epic Games"), empty when none does.
  void SetLaunch(const GameLaunch& launch, bool windows, const std::string& install_path,
                 const QString& store = {});
  GameLaunch Launch() const;

  QLabel* EmptyLabel() const { return empty_; }
  QPushButton* AddButton() const { return add_; }
  // The game's own program, or a "play" option.
  FlatCombo* PlayStarts() const { return default_; }
  // Offer nothing else when Play is clicked; only with an option picked to start.
  Switch* OnlyChoice() const { return only_; }

 signals:
  void Changed();

 private:
  void Rebuild();
  // Edits `option` in a dialog; false if cancelled.
  bool Edit(LaunchOption& option);

  GameLaunch launch_;
  bool windows_ = false;
  std::string install_path_;
  QString store_;
  QVBoxLayout* rows_ = nullptr;
  QLabel* empty_ = nullptr;
  QPushButton* add_ = nullptr;
  FlatCombo* default_ = nullptr;
  Switch* only_ = nullptr;
};

}  // namespace mira_gui
