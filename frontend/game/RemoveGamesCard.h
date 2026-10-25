#pragma once

#include <QString>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

namespace mira_gui {

class ArtworkStore;

// Asks before removing games: what each one loses, by the plans mirad gave (POST
// /v1/games/removal), with one switch for their prefixes, off by default since saves live there,
// and one for their save backups, off by default so a game added back finds them. An in-window
// card over the library.
class RemoveGamesCard : public SettingsCard {
  Q_OBJECT

 public:
  // `games` and `plans` in the same order.
  RemoveGamesCard(std::vector<GameSummary> games, std::vector<RemovalPlan> plans,
                  ArtworkStore* artwork, QWidget* parent = nullptr);

  // The sizes behind the plans, once measured; until then they show as "…".
  void SetSizes(const std::map<std::string, RemovalSize>& sizes);

  // What the action button says, for `plans`: "Uninstall in Steam", "Delete", "Remove 3 games"...
  static QString ActionText(const std::vector<RemovalPlan>& plans);

 signals:
  // Remove them, deleting their prefixes too when `delete_prefix`, and their save backups when
  // `delete_saves`.
  void Confirmed(bool delete_prefix, bool delete_saves);
  void CloseRequested();

 private:
  // A row with a label and muted text on the right, which `text` fills again when sizes land.
  void AddInfoRow(const QString& label, std::function<QString()> text);
  // The same for one game of several: its name gives way to the text, which stays whole.
  void AddGameRow(const QString& name, std::function<QString()> text);
  // The prefix switch, when any game has a prefix that can go.
  void AddPrefixRow();
  // The save backups switch, when any game has backups.
  void AddSavesRow();
  QString Size(const std::string& id, bool prefix) const;

  std::vector<GameSummary> games_;
  std::vector<RemovalPlan> plans_;
  std::map<std::string, RemovalSize> sizes_;
  bool measured_ = false;
  std::vector<std::function<void()>> refresh_;
  Switch* prefix_ = nullptr;
  Switch* saves_ = nullptr;
};

}  // namespace mira_gui
