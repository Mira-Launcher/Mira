#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../client/Types.h"

class QButtonGroup;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QPushButton;
class QStackedWidget;
class QUrl;
class QVariantAnimation;

namespace mira_gui {

class FlowLayout;
class TabRow;

// A grid of one art slot's candidates ("cover", "hero", "logo" or "icon";
// art_candidates in GET .../metadata), shown as previews mirad caches for it
// (POST .../artwork/thumbs), a screenful at a time as the grid scrolls, led by
// None (no art, the placeholder) and the user's own image, in use or chosen, dropped or
// downloaded from a URL.
// Clicking one only previews it; Apply() is what switches the game's art.
class ArtPickerPanel : public QWidget {
  Q_OBJECT

 public:
  // `title`: a store title that isn't installed (its future id), which only has a cover.
  explicit ArtPickerPanel(std::string game_id, QWidget* parent = nullptr, bool title = false);

  // Loads `slot`'s candidates and starts on the one in use.
  void Open(const std::string& slot);
  const std::string& slot() const { return slot_; }

  // True when the pick isn't the candidate already in use.
  bool HasChange() const;
  // True when the pick is no art at all.
  bool PicksNone() const;
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
  // Double-click or Enter on a candidate: the host applies it.
  void PickActivated();
  void ApplyFailed(QString slot, QString error);

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  void ChooseFile();
  // Reads `path` and adds it to the front of the grid as the pick.
  void UseFile(const QString& path);
  // Downloads an http(s) image and does the same.
  void UseUrl(const QUrl& url);
  // Decodes `bytes` off the UI thread and shows them as the pick, named `name`.
  void UseBytes(QByteArray bytes, const QString& name);
  // The loaded image as the "Yours" tile, picked.
  void ShowUpload(const QString& name, const QPixmap& pixmap, QSize size);
  // The leading tiles that aren't candidates: None, and the user's image the slot uses.
  void AddOwnItems(const std::string& source);
  void Populate(const GameMetadataResult& result);
  // Every other tab's count, once: what's cached now, then SteamGridDB's total from its first page.
  void RequestCounts(const GameMetadataResult& result);
  // Added in the first placeholder's place, or at the end.
  QListWidgetItem* AddItem(const ArtCandidate& candidate);
  void UpdateTitle();
  // SteamGridDB's next page of this slot under the chips, if there is one.
  void RequestPage();
  void ShowPage(const ArtCandidatesEvent& event);
  // Trailing placeholders for what SteamGridDB has and isn't loaded yet, so the grid is its
  // whole length from the start.
  void SetPlaceholders(int count);
  void RebuildChips();
  // `style` is SteamGridDB's for `label`, empty for All or art from elsewhere.
  void ApplyFilter(const QString& label, const std::string& style);
  // The chips changed what to ask SteamGridDB for: its pages again, from the first.
  void RestartPaging();
  // Whether the chips (style, adult) let `item` show.
  bool PassesFilter(const QListWidgetItem* item) const;
  void RequestVisible();
  void ShowThumbs(const std::string& slot,
                  const std::vector<std::pair<std::int64_t, QImage>>& images);
  void MarkFailed(const std::vector<std::int64_t>& ids);
  QListWidgetItem* ItemFor(std::int64_t id) const;
  void Pick(QListWidgetItem* item);
  void EmitPreview();
  void UpdateGridSize();
  void UpdatePulse();
  void ShowMessage(const QString& text, bool offer_fetch);
  void ShowNothingFound();
  void LoadMatches(const QString& query);
  void ChooseMatch(int index);
  void HandleEvent(const std::string& type, const std::string& data);

  std::string id_;
  bool title_ = false;
  std::string slot_;
  std::vector<ArtCandidate> candidates_;
  std::optional<std::int64_t> active_id_;
  std::optional<std::int64_t> pick_;
  // Kept across Open() calls, so switching slots back and forth is instant. Candidates' are
  // scaled to the grid's cells; the user's own image is whole.
  std::map<std::pair<std::string, std::int64_t>, QPixmap> thumbs_;
  // The pick's preview at full size, for the host.
  std::optional<std::pair<std::int64_t, QPixmap>> preview_;
  // Between a SteamGridDB refetch and its game.metadata_ready.
  bool fetching_ = false;
  bool matches_loaded_ = false;
  QString filter_;            // the chip's label; empty for all
  std::string filter_style_;  // its SteamGridDB style, empty for All and art from elsewhere
  bool adult_only_ = false;   // the Adult chip: only what SteamGridDB marks adult
  // SteamGridDB's counts of the whole list, from its first page.
  std::vector<std::pair<std::string, int>> style_counts_;
  int adult_count_ = 0;
  bool counts_known_ = false;
  // The user's own image for this slot, as PNG or JPEG bytes for mirad.
  std::string upload_;
  // Paging through SteamGridDB's own results as the grid scrolls.
  int next_page_ = 0;
  bool more_pages_ = true;
  bool page_loading_ = false;
  std::string page_request_;
  int griddb_total_ = -1;  // unknown until the first page
  // RequestCounts' page requests by slot, and each slot's art that isn't SteamGridDB's.
  bool counts_requested_ = false;
  std::map<std::string, std::string> count_requests_;
  std::map<std::string, int> own_counts_;
  // What the pages loaded are for (a style, adult only), and the candidates they brought.
  std::pair<std::string, bool> paged_;
  std::set<std::int64_t> query_loaded_;
  // The slot and candidate an Apply() is waiting to hear back on.
  std::optional<std::pair<std::string, std::int64_t>> applying_;

  TabRow* slots_ = nullptr;  // one tab per slot, each with its count once known
  FlowLayout* chips_layout_ = nullptr;
  QButtonGroup* chips_ = nullptr;
  QWidget* match_row_ = nullptr;
  QComboBox* match_ = nullptr;
  QLineEdit* match_search_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QListWidget* grid_ = nullptr;
  QWidget* message_page_ = nullptr;
  QLabel* message_ = nullptr;
  QPushButton* fetch_button_ = nullptr;
  QPushButton* none_button_ = nullptr;
  QPushButton* url_button_ = nullptr;
  QLineEdit* url_field_ = nullptr;
  QPushButton* adult_chip_ = nullptr;
  QNetworkAccessManager* network_ = nullptr;
  // Drives the pulse drawn on previews still loading.
  QVariantAnimation* pulse_ = nullptr;
  class ThumbDelegate* delegate_ = nullptr;
};

}  // namespace mira_gui
