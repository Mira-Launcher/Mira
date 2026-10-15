#pragma once

#include <QDialog>
#include <QString>
#include <string>

class QLineEdit;
class QPushButton;

namespace mira_gui {

// "Run something inside this game's prefix": `POST /v1/games/{id}/run`.
//
// The executable is picked with a file browser rather than prefilled from a
// folder scan: install_path is usually a Wine prefix full of DLLs and
// support files alongside the real executable, and a detected-candidates
// list there tends to be mostly noise.
//
// Takes install_path/name directly rather than a GameDetail, since both are
// already on GameSummary, so a caller never needs a fresh detail fetch just
// to open this.
class RunInPrefixDialog : public QDialog {
  Q_OBJECT

 public:
  RunInPrefixDialog(std::string game_id, const std::string& install_path, const QString& name,
                    QWidget* parent = nullptr);

 private:
  void Run();

  std::string game_id_;
  std::string install_path_;
  QLineEdit* exe_ = nullptr;
  QLineEdit* args_ = nullptr;
  QPushButton* run_ = nullptr;
};

}  // namespace mira_gui
