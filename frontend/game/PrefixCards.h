#pragma once

#include <QString>
#include <set>
#include <string>

#include "../settings/SettingsCard.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace mira_gui {

class FlowLayout;
class ProgressRail;

// Runs a program in a game's prefix (POST /v1/games/{id}/run), making the prefix first when it
// has none. An in-window card (cards::Show) that closes once the program has started.
class RunInPrefixCard : public SettingsCard {
  Q_OBJECT

 public:
  RunInPrefixCard(std::string game_id, std::string install_path, const QString& name,
                  QWidget* parent = nullptr);

 private:
  void Run();

  std::string game_id_;
  std::string install_path_;
  QLineEdit* program_ = nullptr;
  QLineEdit* args_ = nullptr;
  ProgressRail* rail_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* run_ = nullptr;
};

// Runs winetricks verbs in a game's prefix (POST /v1/games/{id}/tricks), one at a time: common
// verbs as chips, those already in the prefix ticked (GET .../tricks), and a field that searches
// them or takes any other verb. Stays open after a verb finishes, for the next one.
class WinetricksCard : public SettingsCard {
  Q_OBJECT

 public:
  WinetricksCard(std::string game_id, const QString& name, QWidget* parent = nullptr);

 private:
  void Run();
  void HandleEvent(const std::string& type, const std::string& data);
  void SetRunning(bool running);
  void SetStatus(const QString& text, bool error = false);
  void RebuildChips();

  std::string game_id_;
  QString name_;
  std::set<std::string> installed_;
  bool running_ = false;
  QLineEdit* verb_ = nullptr;
  QWidget* chips_ = nullptr;
  FlowLayout* chips_layout_ = nullptr;
  ProgressRail* rail_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* log_ = nullptr;
  QPushButton* run_ = nullptr;
};

}  // namespace mira_gui
