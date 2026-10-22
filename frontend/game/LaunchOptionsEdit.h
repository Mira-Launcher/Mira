#pragma once

#include <QWidget>
#include <string>

#include "../client/Types.h"

class QCheckBox;
class QComboBox;
class QVBoxLayout;

namespace mira_gui {

// A game's launch options as rows with Edit and Remove, an Add row, which one Play starts, and
// whether that's the only choice offered.
class LaunchOptionsEdit : public QWidget {
  Q_OBJECT

 public:
  explicit LaunchOptionsEdit(QWidget* parent = nullptr);

  // `windows`: its options can run in its prefix; `install_path` is where the file picker opens;
  // `store`: the store that starts the game signed in ("Epic Games"), empty when none does.
  void SetLaunch(const GameLaunch& launch, bool windows, const std::string& install_path,
                 const QString& store = {});
  GameLaunch Launch() const;

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
  QWidget* empty_ = nullptr;
  QComboBox* default_ = nullptr;
  QCheckBox* only_ = nullptr;
};

}  // namespace mira_gui
