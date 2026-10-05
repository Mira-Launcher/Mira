#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../client/Types.h"

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QVariantAnimation;

namespace mira_gui {

class TabRow;

// A grid of one art slot's candidates ("cover" or "hero"; art_candidates in
// GET .../metadata), shown as previews mirad caches for it
// (POST .../artwork/thumbs), a screenful at a time as the grid scrolls.
// Clicking one only previews it; Apply() is what switches the game's art.
class ArtPickerPanel : public QWidget {
  Q_OBJECT

public:
  explicit ArtPickerPanel(std::string game_id, QWidget* parent = nullptr);

  // Loads `slot`'s candidates and starts on the one in use.
  void Open(const std::string& slot);
  const std::string& slot() const { return slot_; }

  // True when the pick isn't the candidate already in use.
  bool HasChange() const;
  // Back to the candidate in use, dropping the pick.
  void ResetPick();
  // Switches the slot to the pick. game.artwork_selected follows on success;
  // ApplyFailed on failure.
  void Apply();

signals:
  // The pick's preview, for the host to show in place of the slot's art.
  // Null when the pick is the one in use.
  void Previewed(QString slot, QPixmap preview);
  void PickChanged(bool has_change);
  // The user switched between Covers and Hero art; a pick in the other slot was dropped.
  void SlotChanged(QString slot);
  // Double-click or Enter on a candidate: the host applies it.
  void PickActivated();
  void ApplyFailed(QString slot, QString error);

private:
  void Populate(const GameMetadataResult& result);
  QListWidgetItem* AddItem(const ArtCandidate& candidate);
  void UpdateTitle();
  // SteamGridDB's next page of this slot, if there is one.
  void RequestPage();
  void ShowPage(const ArtCandidatesEvent& event);
  void RebuildChips();
  void ApplyFilter(const QString& style);
  void RequestVisible();
  void ShowThumbs(const std::string& slot, const std::vector<std::pair<std::int64_t, QImage>>& images);
  void MarkFailed(const std::vector<std::int64_t>& ids);
  QListWidgetItem* ItemFor(std::int64_t id) const;
  void Pick(QListWidgetItem* item);
  void EmitPreview();
  void UpdateGridSize();
  void UpdatePulse();
  void ShowMessage(const QString& text, bool offer_fetch);
  void LoadMatches(const QString& query);
  void ChooseMatch(int index);
  void HandleEvent(const std::string& type, const std::string& data);

  std::string id_;
  std::string slot_;
  std::vector<ArtCandidate> candidates_;
  std::optional<std::int64_t> active_id_;
  std::optional<std::int64_t> pick_;
  // Kept across Open() calls, so switching slots back and forth is instant.
  std::map<std::pair<std::string, std::int64_t>, QPixmap> thumbs_;
  // Between a SteamGridDB refetch and its game.metadata_ready.
  bool fetching_ = false;
  bool matches_loaded_ = false;
  QString filter_;  // the chip's style; empty for all
  // Paging through SteamGridDB's own results as the grid scrolls.
  int next_page_ = 0;
  bool more_pages_ = true;
  bool page_loading_ = false;
  std::string page_request_;
  int griddb_total_ = -1;  // unknown until the first page
  // The slot and candidate an Apply() is waiting to hear back on.
  std::optional<std::pair<std::string, std::int64_t>> applying_;

  TabRow* slots_ = nullptr;  // Covers and Hero art, each with its count once known
  QHBoxLayout* chips_layout_ = nullptr;
  QButtonGroup* chips_ = nullptr;
  QWidget* match_row_ = nullptr;
  QComboBox* match_ = nullptr;
  QLineEdit* match_search_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QListWidget* grid_ = nullptr;
  QWidget* message_page_ = nullptr;
  QLabel* message_ = nullptr;
  QPushButton* fetch_button_ = nullptr;
  // Drives the pulse drawn on previews still loading.
  QVariantAnimation* pulse_ = nullptr;
};

}  // namespace mira_gui
