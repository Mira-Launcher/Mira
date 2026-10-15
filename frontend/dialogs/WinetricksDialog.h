#pragma once

#include <QDialog>
#include <QString>
#include <string>

class QComboBox;
class QLabel;
class QPushButton;

namespace mira_gui {

// Runs a winetricks verb inside a game's prefix. 202-then-SSE like
// runner downloads; stays open after a verb finishes so another
// can be run without reopening.
class WinetricksDialog : public QDialog {
  Q_OBJECT

 public:
  WinetricksDialog(std::string game_id, QString game_name, QWidget* parent = nullptr);

 private:
  void Run();
  void HandleEvent(const std::string& type, const std::string& data);
  void SetStatus(const QString& text, bool error = false);

  std::string game_id_;
  QComboBox* verb_ = nullptr;
  QPushButton* run_ = nullptr;
  QLabel* status_ = nullptr;
};

}  // namespace mira_gui
