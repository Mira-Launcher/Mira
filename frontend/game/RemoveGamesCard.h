#pragma once

#include <QString>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../components/Switch.h"
#include "LiveCard.h"

namespace mira_gui {

class ArtworkStore;

// Asks before removing games: what each one loses, by the plans mirad gave (POST
// /v1/games/removal), with one switch for their prefixes, off by default since saves live there,
// and one for their save backups, off by default so a game added back finds them. An in-window
// card over the library.
class RemoveGamesCard : public LiveCard {
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
  // Remove them, deleting their prefixes too when `delete_prefix`, their save backups when
  // `delete_saves`, and their files outside Mira's folders when `delete_outside`.
  void Confirmed(bool delete_prefix, bool delete_saves, bool delete_outside);
  void CloseRequested();

 private:
  // An info row for one game of several: its name gives way to the text, which stays whole.
  void AddGameRow(const QString& name, std::function<QString()> text);
  // A switch, off by default, for what the games `has` picks lose, with their total `field` size;
  // nullptr when none has any. Files outside Mira's folders, prefixes and save backups use it.
  Switch* AddSizedSwitch(const QString& label, const std::function<bool(const RemovalPlan&)>& has,
                         std::uint64_t RemovalSize::* field);
  QString Size(const std::string& id, bool prefix) const;

  std::vector<GameSummary> games_;
  std::vector<RemovalPlan> plans_;
  std::map<std::string, RemovalSize> sizes_;
  bool measured_ = false;
  Switch* outside_ = nullptr;
  Switch* prefix_ = nullptr;
  Switch* saves_ = nullptr;
};

}  // namespace mira_gui
