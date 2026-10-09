#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../sources/ManageSourcesCard.h"
#include "SidebarGames.h"
#include "SidebarStyleCard.h"

class QIcon;
class QLabel;
class QMenu;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class GameLibraryModel;
struct SourceInfo;

// The window's left side: Library, Runners and Settings, then (scrolling)
// PINNED, SOURCES and RECENTLY PLAYED, then Add games, fetch missing art and
// the footer. Source rows drag to reorder, and only show once their source is
// set up (signed in, installed, or with games) and not hidden or turned off.
class Sidebar : public QWidget {
  Q_OBJECT

 public:
  // `prefs` is frontend.toml as read at startup.
  Sidebar(GameLibraryModel* library, ArtworkStore* artwork, const FrontendPrefs& prefs,
          QWidget* parent = nullptr);

  // What Settings saved: which sources show and in what order, the source
  // rows' counts and covers, and how PINNED and RECENTLY PLAYED look.
  void ApplyPrefs(const FrontendPrefs& prefs);
  // Highlights the row of the page on screen; none of them for Settings.
  void SetActive(bool library, bool runners, bool tags, const QString& source_id);
  // Hidden pins show only while the grid shows the Hidden filter.
  void SetShowingHidden(bool showing_hidden);
  void SetFooter(int shown, int total, bool mirad_reachable);
  // Add games, Settings and fetch art, which act on a grid that's covered.
  void SetActionsEnabled(bool enabled);

  // Hides the sources turned off in Settings (`<id>.enabled`), and asks
  // which stores are signed in and which launchers installed.
  void RefreshSources();
  std::vector<ManageSourcesCard::Entry> SourceEntries() const;
  void SetSourceHidden(const QString& id, bool hidden);
  // Shows and stores a new order.
  void SetSourceOrder(std::vector<QString> order);
  // Turns a source on or off in mirad's config.
  void SetSourceEnabled(const QString& id, bool enabled);
  void NoteImported(const QString& id);
  // A source was removed: drop its games and turn its row off.
  void ForgetSource(const QString& id);

  // What PINNED lists.
  std::vector<const GameSummary*> PinnedGames() const;
  // The Library row's height, which Settings' back row matches.
  int FirstRowHeight() const;
  SidebarStyleCard::Choices StyleChoices() const;
  // Redraws both sections and stores the choices.
  void SetStyleChoices(const SidebarStyleCard::Choices& choices);
  // Marks these games' pinned and recently played rows as selected, as they are in the grid.
  void SetSelectedGames(const QSet<QString>& ids);

 signals:
  void LibraryClicked();
  void RunnersClicked();
  void TagsClicked();
  // Empty `focus_key` opens Settings at its start.
  void SettingsRequested(const QString& focus_key);
  void SourceClicked(const SourceInfo& source);
  void ManageSourcesRequested();
  void StyleRequested();
  void FetchArtRequested();
  // A pinned or recently played row's click.
  void PlayRequested(const std::string& id);
  void GameMenuRequested(const std::string& id, const QPoint& global_pos);
  // Ctrl+click on a game row: add it to the selection, or take it out.
  void SelectionToggled(const std::string& id);
  // A game row's hover card after its dwell (`anchor` is global), and its end.
  void HoverRequested(const std::string& id, const QRect& anchor, const QString& hint);
  void HoverEnded();
  // Something Manage sources shows changed.
  void SourcesChanged();
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  QWidget* BuildGameHeading(QWidget* parent, const QString& text, QToolButton*& button);
  QMenu* BuildAddGamesMenu();
  // Redrawn rather than stored: each glyph is painted in the theme's colors.
  void ApplyIcons();
  // The rows' checked state and icons, for the page SetActive last named.
  void ShowActive();
  // Greys out and moves down the sources with nothing set up yet.
  void UpdateSources();
  std::vector<QString> SourceOrder() const;
  // Moves `id` to just before the visible row `before` (end if -1).
  void MoveSource(const QString& id, int before);
  int SourceDropRow(int y) const;
  void ShowSourceMenu(const SourceInfo& source, const QPoint& global_pos);
  void ShowMenu(const QPoint& global_pos);
  // Both sections, if what they'd show changed.
  void RefreshGames();
  // Rebuilds one section's rows in `style`, only if what they'd show differs
  // from `signature`. `recent` rows say when each was last played. Placeholders
  // fill the section up to `places`; with no games and none, `empty_text` shows.
  void FillSection(QWidget* heading, QVBoxLayout* layout,
                   const std::vector<const GameSummary*>& games, sidebar::Style style, bool recent,
                   int places, const QString& empty_text, QString& signature);
  // A row or cover's click, menu and hover card.
  void WireGame(QPushButton* row, const GameSummary& game);
  // A game row is drawn from this game's art.
  bool ShowsArtOf(const QString& id) const;

  GameLibraryModel* library_ = nullptr;
  ArtworkStore* artwork_ = nullptr;

  QPushButton* library_nav_ = nullptr;
  QPushButton* runners_nav_ = nullptr;
  QPushButton* tags_nav_ = nullptr;
  QPushButton* settings_button_ = nullptr;
  QToolButton* manage_sources_button_ = nullptr;
  QToolButton* add_games_ = nullptr;
  QToolButton* fetch_art_button_ = nullptr;
  QLabel* footer_ = nullptr;
  bool library_active_ = true;
  bool runners_active_ = false;
  bool tags_active_ = false;
  QString active_source_;

  // One row per AllSources() entry, same order.
  QList<QPushButton*> source_navs_;
  // The parts each source row lays out in place of its own text: the deck of covers (or a
  // dot), the name with a line under it, and the trailing count shown only without covers.
  QList<QLabel*> source_decks_;
  QList<QLabel*> source_names_;
  QList<QLabel*> source_details_;
  QList<QLabel*> source_counts_;
  QVBoxLayout* source_nav_layout_ = nullptr;
  QLabel* sources_empty_ = nullptr;    // shown while no source row is
  QSet<QString> hidden_sources_;       // unticked "In sidebar"
  QSet<QString> disabled_sources_;     // <id>.enabled = false
  std::vector<QString> source_order_;  // saved order; see SourceOrder()
  // Store signed in / launcher installed, by source id, as last asked.
  QHash<QString, bool> source_ready_;
  QHash<QString, QString> source_account_;     // signed-in account, where a store says
  QHash<QString, qint64> source_imported_at_;  // last import, unix seconds
  bool show_source_counts_ = true;
  bool show_source_covers_ = true;
  QSet<QString> source_cover_ids_;  // the games source rows show covers of
  QWidget* source_nav_container_ = nullptr;  // accepts source row drops
  QWidget* source_drop_line_ = nullptr;
  QPushButton* source_drag_row_ = nullptr;
  QPoint source_drag_start_;

  QWidget* pinned_heading_ = nullptr;
  QVBoxLayout* pinned_layout_ = nullptr;
  QString pinned_signature_;  // what the rows show now; see FillSection
  QWidget* recent_heading_ = nullptr;
  QVBoxLayout* recent_layout_ = nullptr;
  QString recent_signature_;
  QToolButton* pinned_customize_ = nullptr;
  QToolButton* recent_customize_ = nullptr;
  SidebarStyleCard::Choices style_;
  bool showing_hidden_ = false;
  // Dwell before a game row's hover card.
  QTimer* hover_timer_ = nullptr;
  QPointer<QWidget> hover_row_;
  QSet<QString> selected_games_;  // as the library grid has them
};

}  // namespace mira_gui
