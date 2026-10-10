#include "Sidebar.h"

#include <QApplication>
#include <QDateTime>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <map>

#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Stores.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/DesktopEntryImportDialog.h"
#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/OwnedTitles.h"
#include "../library/GamePresentation.h"
#include "../library/HoverCard.h"
#include "../library/LibraryActions.h"
#include "../settings/SettingsPanel.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/Scrolling.h"
#include "../widgets/TabRow.h"

namespace mira_gui {
namespace {

constexpr const char* kSourceMime = "application/x-mira-source";

// The heading's spacing sits on its row, so a button beside it centers on the text.
constexpr QMargins kHeadingMargins(0, 14, 0, 2);

QLabel* Heading(QWidget* parent, const QString& text) {
  QLabel* label = MakeGroupHeading(parent, text);
  label->setContentsMargins(kHeadingMargins);
  return label;
}

QLabel* RowHeading(QWidget* parent, const QString& text) {
  return MakeGroupHeading(parent, text);
}

// Saves only the fields set in `prefs`; a failure says what didn't stick.
void SavePrefs(QWidget* parent, const FrontendPrefs& prefs, const QString& failure) {
  api::SaveFrontendPrefsAsync(parent, prefs, [parent, failure](PatchConfigResult result) {
    if (!result.ok) notify::FailedRequest(parent, failure, result.error);
  });
}

}  // namespace

Sidebar::Sidebar(GameLibraryModel* library, ArtworkStore* artwork, const FrontendPrefs& prefs,
                 QWidget* parent)
    : QWidget(parent), library_(library), artwork_(artwork) {
  setObjectName("left_sidebar");
  connect(EventHub::Instance(), &EventHub::Received, this, [this](const std::string& type) {
    if (type == "sources.changed") RefreshSources();
  });

  // Rows with their own menu handle it first; the rest fall through to here.
  setContextMenuPolicy(Qt::CustomContextMenu);
  connect(this, &QWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) { ShowMenu(mapToGlobal(pos)); });
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(2);

  // Always visible (not just a "back" affordance): checked exactly when the
  // grid is the current content.
  library_nav_ = new QPushButton("Library", this);
  library_nav_->setObjectName("library_nav");
  library_nav_->setFlat(true);
  library_nav_->setCheckable(true);
  library_nav_->setChecked(true);
  connect(library_nav_, &QPushButton::clicked, this, &Sidebar::LibraryClicked);
  layout->addWidget(library_nav_);

  runners_nav_ = new QPushButton("Runners", this);
  runners_nav_->setFlat(true);
  runners_nav_->setCheckable(true);
  connect(runners_nav_, &QPushButton::clicked, this, &Sidebar::RunnersClicked);
  layout->addWidget(runners_nav_);

  tags_nav_ = new QPushButton("Tags", this);
  tags_nav_->setFlat(true);
  tags_nav_->setCheckable(true);
  connect(tags_nav_, &QPushButton::clicked, this, &Sidebar::TagsClicked);
  layout->addWidget(tags_nav_);

  settings_button_ = new QPushButton("Settings", this);
  settings_button_->setObjectName("sidebar_settings");
  settings_button_->setFlat(true);
  connect(settings_button_, &QPushButton::clicked, this,
          [this] { emit SettingsRequested(QString()); });
  layout->addWidget(settings_button_);

  // The PINNED, SOURCES and RECENTLY PLAYED rows scroll, so they never set
  // the window's minimum height.
  auto* nav_scroll = new QScrollArea(this);
  nav_scroll->setWidgetResizable(true);
  nav_scroll->setFrameShape(QFrame::NoFrame);
  nav_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_scroll->viewport()->setAutoFillBackground(false);
  SetUpScrolling(nav_scroll);
  auto* nav_content = new QWidget();
  nav_content->setAutoFillBackground(false);
  auto* nav_layout = new QVBoxLayout(nav_content);
  nav_layout->setContentsMargins(0, 0, 0, 0);
  nav_layout->setSpacing(2);

  pinned_heading_ = BuildGameHeading(nav_content, "PINNED", pinned_customize_);
  nav_layout->addWidget(pinned_heading_);
  pinned_layout_ = new QVBoxLayout();
  pinned_layout_->setSpacing(2);
  nav_layout->addLayout(pinned_layout_);

  auto* sources_heading = new QWidget(nav_content);
  auto* sources_heading_layout = new QHBoxLayout(sources_heading);
  sources_heading_layout->setContentsMargins(kHeadingMargins);
  sources_heading_layout->addWidget(RowHeading(sources_heading, "SOURCES"), /*stretch=*/1);
  manage_sources_button_ = new QToolButton(sources_heading);
  manage_sources_button_->setAutoRaise(true);
  manage_sources_button_->setToolTip("Manage sources");
  connect(manage_sources_button_, &QToolButton::clicked, this, &Sidebar::ManageSourcesRequested);
  sources_heading_layout->addWidget(manage_sources_button_, 0, Qt::AlignVCenter);
  nav_layout->addWidget(sources_heading);

  source_nav_layout_ = new QVBoxLayout();
  source_nav_layout_->setSpacing(4);
  // Rows drag to reorder; see eventFilter.
  source_nav_container_ = nav_content;
  nav_content->setAcceptDrops(true);
  nav_content->installEventFilter(this);
  source_drop_line_ = new QWidget(nav_content);
  source_drop_line_->setObjectName("drop_line");
  source_drop_line_->setAttribute(Qt::WA_StyledBackground);
  source_drop_line_->setFixedHeight(2);
  source_drop_line_->hide();
  for (const SourceInfo& source : AllSources()) {
    // The button draws only the background; its parts are labels, so the name can elide and
    // carry a second line.
    auto* nav = new QPushButton(nav_content);
    nav->setFlat(true);
    nav->setCheckable(true);
    nav->setVisible(false);  // until UpdateSources knows it's set up
    nav->setObjectName("source_nav");
    nav->setAccessibleName(source.name);
    auto* row = new QHBoxLayout(nav);
    auto* deck = new QLabel(nav);
    deck->setAlignment(Qt::AlignCenter);
    row->addWidget(deck);
    auto* lines = new QVBoxLayout();
    lines->setSpacing(1);
    lines->addStretch(1);
    auto* name = new ElidedLabel(source.name, nav);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    lines->addWidget(name);
    auto* detail = new QLabel(nav);
    lines->addWidget(detail);
    lines->addStretch(1);
    row->addLayout(lines, /*stretch=*/1);
    auto* count = new QLabel(nav);
    count->setProperty("role", "muted");
    row->addWidget(count);
    for (QLabel* label : nav->findChildren<QLabel*>()) label->setAttribute(Qt::WA_TransparentForMouseEvents);
    source_decks_.append(deck);
    source_names_.append(name);
    source_details_.append(detail);
    source_counts_.append(count);
    connect(nav, &QPushButton::clicked, this, [this, source] { emit SourceClicked(source); });
    nav->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        nav, &QWidget::customContextMenuRequested, this,
        [this, nav, source](const QPoint& pos) { ShowSourceMenu(source, nav->mapToGlobal(pos)); });
    nav->installEventFilter(this);
    source_nav_layout_->addWidget(nav);
    source_navs_.append(nav);
  }
  nav_layout->addLayout(source_nav_layout_);
  sources_empty_ = MakeLabel(nav_content, "No sources currently enabled", "muted");
  sources_empty_->setContentsMargins(8, 2, 8, 2);
  nav_layout->addWidget(sources_empty_);

  recent_heading_ = BuildGameHeading(nav_content, "RECENTLY PLAYED", recent_customize_);
  nav_layout->addWidget(recent_heading_);
  recent_layout_ = new QVBoxLayout();
  recent_layout_->setSpacing(2);
  nav_layout->addLayout(recent_layout_);

  nav_layout->addStretch(1);
  nav_scroll->setWidget(nav_content);
  layout->addWidget(nav_scroll, /*stretch=*/1);

  auto* actions = new QHBoxLayout();
  actions->setSpacing(6);
  add_games_ = new QToolButton(this);
  add_games_->setObjectName("add_games");
  add_games_->setText("Add games");
  add_games_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  add_games_->setPopupMode(QToolButton::InstantPopup);
  // QToolButton stays content-sized otherwise.
  add_games_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  add_games_->setMenu(BuildAddGamesMenu());
  actions->addWidget(add_games_, /*stretch=*/1);

  fetch_art_button_ = new QToolButton(this);
  fetch_art_button_->setObjectName("fetch_art");
  // mirad only fetches on its own for a newly found game, so one that failed
  // once stays bare until asked again.
  fetch_art_button_->setToolTip("Fetch missing cover art for every game without one");
  connect(fetch_art_button_, &QToolButton::clicked, this, &Sidebar::FetchArtRequested);
  actions->addWidget(fetch_art_button_);
  layout->addSpacing(6);
  layout->addLayout(actions);

  footer_ = new QLabel(this);
  footer_->setProperty("role", "muted");
  footer_->setContentsMargins(6, 8, 6, 2);
  footer_->setWordWrap(true);
  layout->addWidget(footer_);

  ApplyIcons();
  connect(library_, &GameLibraryModel::Changed, this, [this] {
    UpdateSources();
    RefreshGames();
  });
  // Game rows draw their cover, icon or hero, or take their color from it.
  const auto art_changed = [this](const QString& id) {
    if (ShowsArtOf(id)) RefreshGames();
    if (source_cover_ids_.contains(id)) UpdateSources();
  };
  connect(artwork_, &ArtworkStore::CoverChanged, this, art_changed);
  connect(artwork_, &ArtworkStore::SlotArtChanged, this, art_changed);
  // The rows and their icons are drawn in theme colors.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    ApplyIcons();
    RefreshGames();
    ShowActive();
  });
}

// PINNED and RECENTLY PLAYED headings carry a button for the customize card.
QWidget* Sidebar::BuildGameHeading(QWidget* parent, const QString& text, QToolButton*& button) {
  auto* heading = new QWidget(parent);
  auto* heading_layout = new QHBoxLayout(heading);
  heading_layout->setContentsMargins(kHeadingMargins);
  heading_layout->addWidget(RowHeading(heading, text), /*stretch=*/1);
  button = new QToolButton(heading);
  button->setAutoRaise(true);
  button->setToolTip("Customize how these look");
  connect(button, &QToolButton::clicked, this, &Sidebar::StyleRequested);
  heading_layout->addWidget(button, 0, Qt::AlignVCenter);
  heading->setVisible(false);
  return heading;
}

QMenu* Sidebar::BuildAddGamesMenu() {
  auto* menu = new QMenu(add_games_);
  menu->addAction("Scan library folders", this, [this] { actions::ScanLibrary(window()); });
  menu->addAction("Import Steam library", this,
                  [this] { actions::ImportSteam(window(), [this] { NoteImported("steam"); }); });
  menu->addAction("Import Lutris games", this,
                  [this] { actions::ImportLutris(window(), [this] { NoteImported("lutris"); }); })
      ->setToolTip(
          "Add the Wine games from Lutris's database. Nothing is moved or renamed, so the games "
          "stay playable in Lutris too.");
  menu->addAction("Import desktop entries…", this,
                  [this] {
                    DesktopEntryImportDialog dialog(window());
                    dialog.exec();
                  })
      ->setToolTip(
          "Pick installed apps from your application menu to add as games. This includes "
          "Flatpak apps.");
  menu->addSeparator();
  menu->addAction("Add game manually…", this, [this] {
    AddManualGameDialog dialog(window());
    dialog.exec();
  });
  return menu;
}

void Sidebar::ApplyPrefs(const FrontendPrefs& prefs) {
  show_source_counts_ = prefs.sidebar_source_counts.value_or(true);
  show_source_covers_ = prefs.sidebar_source_covers.value_or(true);
  style_.pinned = sidebar::ParseStyle(prefs.sidebar_pinned_style.value_or("covers"));
  style_.recent = sidebar::ParseStyle(prefs.sidebar_recent_style.value_or("covers"));
  style_.recent_count = prefs.sidebar_recent_count.value_or(0);
  style_.recent_when = prefs.sidebar_recent_when.value_or(true);
  UpdateSources();
  RefreshGames();
}

void Sidebar::SetActive(bool library, bool runners, bool tags, const QString& source_id) {
  library_active_ = library;
  runners_active_ = runners;
  tags_active_ = tags;
  active_source_ = source_id;
  ShowActive();
}

void Sidebar::ShowActive() {
  using icons::Glyph;
  const theme::Tokens& tokens = theme::Current();
  library_nav_->setChecked(library_active_);
  library_nav_->setIcon(icons::For(Glyph::Home, library_active_ ? tokens.on_accent : tokens.text));
  runners_nav_->setChecked(runners_active_);
  runners_nav_->setIcon(
      icons::For(Glyph::Wrench, runners_active_ ? tokens.on_accent : tokens.text));
  tags_nav_->setChecked(tags_active_);
  tags_nav_->setIcon(icons::For(Glyph::Tag, tags_active_ ? tokens.on_accent : tokens.text));
  const std::vector<SourceInfo>& sources = AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const bool active = sources[i].id == active_source_;
    source_navs_[i]->setChecked(active);
    // Without covers, a dot in the source's color leads the row.
    if (!show_source_covers_) {
      source_decks_[i]->setPixmap(icons::For(Glyph::Dot, active ? tokens.on_accent : sources[i].color)
                                      .pixmap(QSize(18, 18), devicePixelRatioF()));
    }
    const QString on_accent = QString("color: %1;").arg(tokens.on_accent.name());
    source_names_[i]->setStyleSheet(active ? on_accent + " font-weight: 600;" : "font-weight: 600;");
    source_counts_[i]->setStyleSheet(active ? on_accent : QString());
    QColor detail = active ? tokens.on_accent : tokens.text_muted;
    if (active) detail.setAlpha(200);
    source_details_[i]->setStyleSheet(QString("color: %1; font-size: %2px;")
                                          .arg(theme::ColorToQss(detail))
                                          .arg(tokens.font_size_small));
  }
}

void Sidebar::SetShowingHidden(bool showing_hidden) {
  if (showing_hidden == showing_hidden_) return;
  showing_hidden_ = showing_hidden;
  RefreshGames();
}

void Sidebar::SetFooter(int shown, int total, bool mirad_reachable) {
  // The connection only earns a line when it's gone.
  QString text = QString("%1 of %2 games shown").arg(shown).arg(total);
  if (!mirad_reachable) {
    text += "<br>" + StatusDot(theme::Current().error) + "mirad isn't running";
  }
  footer_->setText(text);
}

void Sidebar::SetActionsEnabled(bool enabled) {
  for (QWidget* control :
       std::initializer_list<QWidget*>{add_games_, settings_button_, fetch_art_button_}) {
    control->setEnabled(enabled);
  }
}

void Sidebar::ApplyIcons() {
  using icons::Glyph;
  const theme::Tokens& tokens = theme::Current();
  settings_button_->setIcon(icons::For(Glyph::Settings));
  add_games_->setIcon(icons::For(Glyph::Plus, tokens.on_accent));
  fetch_art_button_->setIcon(icons::For(Glyph::Image));
  for (QToolButton* button : {manage_sources_button_, pinned_customize_, recent_customize_}) {
    button->setIcon(icons::For(Glyph::Sliders, tokens.text_muted));
  }}

bool Sidebar::eventFilter(QObject* watched, QEvent* event) {
  if (auto* nav = qobject_cast<QPushButton*>(watched);
      nav != nullptr && source_navs_.contains(nav)) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
      source_drag_row_ = nav;
      source_drag_start_ = mouse->position().toPoint();
    } else if (event->type() == QEvent::MouseMove && source_drag_row_ == nav &&
               (mouse->buttons() & Qt::LeftButton) &&
               (mouse->position().toPoint() - source_drag_start_).manhattanLength() >=
                   QApplication::startDragDistance()) {
      const qsizetype index = source_navs_.indexOf(nav);
      auto* mime = new QMimeData();
      mime->setData(kSourceMime, AllSources()[index].id.toUtf8());
      auto* drag = new QDrag(nav);
      drag->setMimeData(mime);
      drag->setPixmap(nav->grab());
      drag->setHotSpot(source_drag_start_);
      source_drag_row_ = nullptr;
      nav->setDown(false);
      drag->exec(Qt::MoveAction);
      return true;
    }
  }
  if (watched == source_nav_container_) {
    const auto* drop = static_cast<QDropEvent*>(event);
    switch (event->type()) {
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        event->accept();
        const int row = SourceDropRow(drop->position().toPoint().y());
        QWidget* anchor = nullptr;
        for (QPushButton* nav : source_navs_) {
          if (nav->isVisible() && source_nav_layout_->indexOf(nav) == row) anchor = nav;
        }
        int y = 0;
        if (anchor != nullptr) {
          y = anchor->geometry().top() - 2;
        } else {
          for (QPushButton* nav : source_navs_) {
            if (nav->isVisible()) y = std::max(y, nav->geometry().bottom());
          }
        }
        source_drop_line_->setGeometry(0, y, source_nav_container_->width(), 2);
        source_drop_line_->show();
        source_drop_line_->raise();
        return true;
      }
      case QEvent::DragLeave:
        source_drop_line_->hide();
        return true;
      case QEvent::Drop: {
        source_drop_line_->hide();
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        const QString id = QString::fromUtf8(drop->mimeData()->data(kSourceMime));
        MoveSource(id, SourceDropRow(drop->position().toPoint().y()));
        event->accept();
        return true;
      }
      default:
        break;
    }
  }
  // Ctrl+click selects instead of playing, as in the grid.
  if (watched->property("hover_game").isValid() &&
      (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
       event->type() == QEvent::MouseButtonDblClick)) {
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier)) {
      if (event->type() == QEvent::MouseButtonPress) {
        emit SelectionToggled(watched->property("hover_game").toString().toStdString());
      }
      return true;
    }
  }
  // A game row shows its game's hover card, after a tile's dwell.
  if (watched->property("hover_game").isValid()) {
    if (event->type() == QEvent::Enter) {
      if (hover_timer_ == nullptr) {
        hover_timer_ = new QTimer(this);
        hover_timer_->setSingleShot(true);
        hover_timer_->setInterval(card::kDwellMs);
        connect(hover_timer_, &QTimer::timeout, this, [this] {
          if (hover_row_ == nullptr) return;
          const GameSummary* game =
              library_->Find(hover_row_->property("hover_game").toString().toStdString());
          if (game == nullptr) return;
          const QString hint = game->running             ? "Right-click to stop it."
                               : game->status == "ready" ? "Click to play."
                                                         : QString();
          emit HoverRequested(
              game->id, QRect(hover_row_->mapToGlobal(QPoint(0, 0)), hover_row_->size()), hint);
        });
      }
      hover_row_ = qobject_cast<QWidget*>(watched);
      hover_timer_->start();
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
      if (hover_timer_ != nullptr) hover_timer_->stop();
      emit HoverEnded();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void Sidebar::RefreshSources() {
  api::ListSourcesAsync(this, [this](SourcesResult result) {
    if (!result.ok) return;
    added_sources_.clear();
    disabled_sources_.clear();
    hidden_sources_.clear();
    source_order_.clear();
    for (const SourceState& source : result.sources) {
      const QString id = QString::fromStdString(source.id);
      source_order_.push_back(id);
      if (source.added) added_sources_.insert(id);
      if (!source.enabled) disabled_sources_.insert(id);
      if (!source.in_sidebar) hidden_sources_.insert(id);
      source_imported_at_[id] = source.imported_at;
    }
    UpdateSources();
    emit SourcesChanged();
  });
  // A store counts as set up once signed in, a launcher once installed.
  for (const SourceInfo& source : AllSources()) {
    if (source.kind != SourceInfo::Kind::Store) continue;
    const QString id = source.id;
    api::GetStoreStatusAsync(this, id.toStdString(), [this, id](StoreStatusResult status) {
      if (!status.ok) return;  // a failed request says nothing about the account
      source_ready_[id] = status.authenticated;
      source_account_[id] = QString::fromStdString(status.account);
      UpdateSources();
    });
  }
  api::GetLaunchersAsync(this, [this](LaunchersResult result) {
    if (!result.ok) return;
    for (const LauncherInfo& launcher : result.launchers) {
      source_ready_[QString::fromStdString(launcher.id)] = launcher.installed;
    }
    UpdateSources();
  });
}

void Sidebar::SetOwnedTitles(OwnedTitles* titles) {
  owned_titles_ = titles;
  connect(owned_titles_, &OwnedTitles::Changed, this, &Sidebar::UpdateSources);
  UpdateSources();
}

void Sidebar::UpdateSources() {
  std::map<std::string, int> owned;
  if (owned_titles_ != nullptr) {
    for (const StoreTitle& title : owned_titles_->Titles()) {
      if (title.owned) ++owned[title.source];
    }
  }
  std::map<std::string, int> counts;
  // Each source's visible games, most recently played first, for its row's covers.
  std::map<std::string, std::vector<const GameSummary*>> shown;
  for (const GameSummary& game : library_->Games()) {
    const std::string source = SourceIdOf(game.source);
    ++counts[source];
    if (!IsHidden(game)) shown[source].push_back(&game);
  }
  constexpr size_t kCovers = 3;
  source_cover_ids_.clear();

  const std::vector<SourceInfo>& sources = AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const QString& id = sources[i].id;
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    // Out of what the store owns, once listed; games it doesn't list still count as owned.
    const auto owns = owned.find(id.toStdString());
    const int of = owns == owned.end() ? 0 : std::max(owns->second, games);
    const QString counted = of > 0 ? QString("%1/%2").arg(games).arg(of) : QString::number(games);
    // Listed once set up; the Sources page adds the rest.
    const bool added = id == "local" || added_sources_.contains(id);
    source_navs_[i]->setVisible(added && !hidden_sources_.contains(id));
    // An off source's games aren't in the library, so it shows that instead of a count.
    const bool off = disabled_sources_.contains(id);
    // A store with games whose account is signed out wants a look.
    const bool signed_out = sources[i].kind == SourceInfo::Kind::Store && games > 0 &&
                            source_ready_.contains(id) && !source_ready_.value(id);
    auto* row = qobject_cast<QHBoxLayout*>(source_navs_[i]->layout());
    if (show_source_covers_) {
      std::vector<const GameSummary*> covers = std::move(shown[id.toStdString()]);
      const auto order = [](const GameSummary* a, const GameSummary* b) {
        if (a->last_played_at.value_or(0) != b->last_played_at.value_or(0))
          return a->last_played_at.value_or(0) > b->last_played_at.value_or(0);
        return a->name < b->name;
      };
      std::ranges::partial_sort(covers, covers.begin() + std::min(kCovers, covers.size()), order);
      covers.resize(std::min(kCovers, covers.size()));
      for (const GameSummary* game : covers) source_cover_ids_.insert(QString::fromStdString(game->id));
      source_navs_[i]->setFixedHeight(52);
      row->setContentsMargins(6, 3, 10, 3);
      row->setSpacing(10);
      source_decks_[i]->setFixedSize(sidebar::kDeckSize);
      source_decks_[i]->setPixmap(
          sidebar::CoverDeck(covers, artwork_, source_navs_[i]->devicePixelRatioF()));
      QString detail = games == 0 && of == 0 ? QString("No games")
                       : show_source_counts_ ? counted + (std::max(games, of) == 1 ? " game" : " games")
                                             : QString();
      if (off) detail = "Off";
      if (signed_out && !off) {
        detail = QString("<span style=\"color: %1;\">Signed out</span>").arg(theme::Current().warning.name()) +
                 (detail.isEmpty() ? QString() : " · " + detail);
      }
      source_details_[i]->setText(detail);
      source_details_[i]->setVisible(!detail.isEmpty());
      source_counts_[i]->hide();
    } else {
      source_navs_[i]->setFixedHeight(38);
      row->setContentsMargins(10, 0, 10, 0);
      row->setSpacing(10);
      source_decks_[i]->setFixedSize(18, 18);  // ShowActive draws the dot
      source_details_[i]->hide();
      QString label = show_source_counts_ ? counted : QString();
      if (off) label = "Off";
      if (signed_out && !off) label = StatusDot(theme::Current().warning) + label;
      source_counts_[i]->setText(label);
      source_counts_[i]->show();
    }
    source_navs_[i]->setToolTip(signed_out ? QString("Signed out of %1").arg(sources[i].name)
                                           : QString());
  }
  // isHidden, not isVisible: this also runs while the sidebar itself is off screen.
  sources_empty_->setVisible(
      std::ranges::all_of(source_navs_, [](const QPushButton* nav) { return nav->isHidden(); }));
  int row = 0;
  for (const QString& id : SourceOrder()) {
    for (int i = 0; i < static_cast<int>(sources.size()); ++i) {
      if (sources[i].id != id) continue;
      source_nav_layout_->removeWidget(source_navs_[i]);
      source_nav_layout_->insertWidget(row++, source_navs_[i]);
    }
  }
  ShowActive();
  emit SourcesChanged();
}

std::vector<SourceEntry> Sidebar::SourceEntries() const {
  std::map<std::string, int> counts;
  for (const GameSummary& game : library_->Games()) ++counts[SourceIdOf(game.source)];
  std::vector<SourceEntry> entries;
  for (const QString& id : SourceOrder()) {
    const SourceInfo* source = FindSourceInfo(id);
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    entries.push_back({.source = *source,
                       .ready = id == "local" || added_sources_.contains(id),
                       .enabled = !disabled_sources_.contains(id),
                       .games = games,
                       .in_sidebar = !hidden_sources_.contains(id),
                       .account = source_account_.value(id).toStdString(),
                       .imported_at = source_imported_at_.value(id, 0)});
  }
  return entries;
}

void Sidebar::NoteImported(const QString&) { RefreshSources(); }

void Sidebar::SetSourceEnabled(const QString& id, bool enabled) {
  if (enabled) {
    disabled_sources_.remove(id);
  } else {
    disabled_sources_.insert(id);
  }
  UpdateSources();
  api::PatchSourceAsync(this, id.toStdString(), enabled, std::nullopt,
                        [this](PatchConfigResult result) {
                          if (!result.ok) {
                            notify::FailedRequest(this, "Could not change that source.",
                                                  result.error);
                          }
                          RefreshSources();
                        });
}

void Sidebar::ForgetSource(const QString& id) {
  added_sources_.remove(id);
  disabled_sources_.insert(id);
  source_ready_[id] = false;
  // mirad's game.removed events say the same; this just doesn't wait for them.
  library_->RemoveSource(id.toStdString());
  RefreshSources();
}

std::vector<QString> Sidebar::SourceOrder() const { return OrderSources(source_order_); }

int Sidebar::SourceDropRow(int y) const {
  // The layout row of the first visible source whose middle is below `y`.
  int best = -1;
  int best_top = 0;
  for (QPushButton* nav : source_navs_) {
    if (!nav->isVisible() || y >= nav->geometry().center().y()) continue;
    if (best == -1 || nav->geometry().top() < best_top) {
      best = source_nav_layout_->indexOf(nav);
      best_top = nav->geometry().top();
    }
  }
  return best;
}

void Sidebar::MoveSource(const QString& id, int before) {
  // Placed right after the visible row that ends up above it (or right before
  // the first one), not next to rows the sidebar hides, so Settings and
  // the Sources page, which list every source, show it where it was put.
  QString above;
  QString first;
  const int end = before >= 0 ? before : source_nav_layout_->count();
  for (int row = 0; row < source_nav_layout_->count(); ++row) {
    auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(row)->widget());
    if (nav == nullptr || !nav->isVisible()) continue;
    const QString nav_id = AllSources()[source_navs_.indexOf(nav)].id;
    if (row == before && nav_id == id) return;  // dropped onto itself
    if (nav_id == id) continue;
    if (first.isEmpty()) first = nav_id;
    if (row < end) above = nav_id;
  }
  std::vector<QString> order = SourceOrder();
  std::erase(order, id);
  auto at = order.begin();
  if (!above.isEmpty()) {
    at = std::ranges::find(order, above) + 1;
  } else if (!first.isEmpty()) {
    at = std::ranges::find(order, first);
  }
  order.insert(at, id);
  if (order == SourceOrder()) return;
  SetSourceOrder(std::move(order));
}

void Sidebar::SetSourceOrder(std::vector<QString> order) {
  source_order_ = order;
  UpdateSources();
  std::vector<std::string> ids;
  for (const QString& source : order) ids.push_back(source.toStdString());
  api::SetSourceOrderAsync(this, ids, [this](PatchConfigResult result) {
    if (!result.ok) notify::FailedRequest(this, "Could not save the sources' order.", result.error);
  });
}

void Sidebar::SetSourceHidden(const QString& id, bool hidden) {
  if (hidden) {
    hidden_sources_.insert(id);
  } else {
    hidden_sources_.remove(id);
  }
  UpdateSources();
  api::PatchSourceAsync(this, id.toStdString(), std::nullopt, !hidden,
                        [this](PatchConfigResult result) {
                          if (!result.ok) {
                            notify::FailedRequest(
                                this, "Could not save which sources the sidebar shows.", result.error);
                          }
                          RefreshSources();
                        });
}

void Sidebar::ShowSourceMenu(const SourceInfo& source, const QPoint& global_pos) {
  QMenu menu(this);
  QAction* open = menu.addAction("Open " + source.name);
  // Neighbours among the visible rows, in their shown order.
  std::vector<QPushButton*> shown;
  for (int row = 0; row < source_nav_layout_->count(); ++row) {
    auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(row)->widget());
    if (nav != nullptr && nav->isVisible()) shown.push_back(nav);
  }
  QPushButton* self = source_navs_[std::ranges::find(AllSources(), source.id, &SourceInfo::id) -
                                   AllSources().begin()];
  const auto position = std::ranges::find(shown, self);
  QAction* up = menu.addAction("Move up");
  up->setEnabled(position != shown.end() && position != shown.begin());
  QAction* down = menu.addAction("Move down");
  down->setEnabled(position != shown.end() && position + 1 != shown.end());
  QAction* hide = menu.addAction("Hide from sidebar");
  menu.addSeparator();
  QAction* manage = menu.addAction("Manage sources…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == open) {
    emit SourceClicked(source);
  } else if (chosen == up) {
    MoveSource(source.id, source_nav_layout_->indexOf(*(position - 1)));
  } else if (chosen == down) {
    MoveSource(source.id,
               position + 2 == shown.end() ? -1 : source_nav_layout_->indexOf(*(position + 2)));
  } else if (chosen == hide) {
    SetSourceHidden(source.id, true);
  } else if (chosen == manage) {
    emit ManageSourcesRequested();
  } else if (chosen == settings) {
    emit SettingsRequested(SettingsPanel::kSidebarSourcesKey);
  }
}

void Sidebar::ShowMenu(const QPoint& global_pos) {
  QMenu menu(this);
  QAction* manage = menu.addAction("Manage sources…");
  QAction* customize = menu.addAction("Customize pinned and recent…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == manage) {
    emit ManageSourcesRequested();
  } else if (chosen == customize) {
    emit StyleRequested();
  } else if (chosen == settings) {
    emit SettingsRequested(SettingsPanel::kSidebarKey);
  }
}

int Sidebar::FirstRowHeight() const { return library_nav_->sizeHint().height(); }

std::vector<const GameSummary*> Sidebar::PinnedGames() const {
  // By name, matching the grid: hidden pins only under the Hidden filter.
  std::vector<const GameSummary*> pinned;
  for (const GameSummary& game : library_->Games()) {
    if (IsPinned(game) && IsHidden(game) == showing_hidden_) pinned.push_back(&game);
  }
  std::ranges::sort(pinned, [](const GameSummary* a, const GameSummary* b) {
    return QString::compare(QString::fromStdString(a->name), QString::fromStdString(b->name),
                            Qt::CaseInsensitive) < 0;
  });
  return pinned;
}

SidebarStyleCard::Choices Sidebar::StyleChoices() const {
  return style_;
}

void Sidebar::SetStyleChoices(const SidebarStyleCard::Choices& choices) {
  style_ = choices;
  RefreshGames();
  FrontendPrefs prefs;
  prefs.sidebar_pinned_style = sidebar::StyleKey(style_.pinned);
  prefs.sidebar_recent_style = sidebar::StyleKey(style_.recent);
  prefs.sidebar_recent_count = style_.recent_count;
  prefs.sidebar_recent_when = style_.recent_when;
  // Shown already, so a failed write would otherwise only surface as the old look after a restart.
  SavePrefs(this, prefs, "Could not save the sidebar's look.");
}

bool Sidebar::ShowsArtOf(const QString& id) const {
  return pinned_signature_.contains(id) || recent_signature_.contains(id);
}

void Sidebar::RefreshGames() {
  const std::vector<const GameSummary*> pinned = PinnedGames();
  const std::vector<const GameSummary*> recent = library_->RecentlyPlayed(style_.recent_count, showing_hidden_);
  // Shown all the time, so their full images stay.
  QSet<QString> kept;
  for (const auto* games : {&pinned, &recent}) {
    for (const GameSummary* game : *games) kept.insert(QString::fromStdString(game->id));
  }
  artwork_->Keep(kept);
  FillSection(pinned_heading_, pinned_layout_, pinned, style_.pinned, /*recent=*/false,
              /*places=*/0, "Nothing currently pinned", pinned_signature_);
  FillSection(recent_heading_, recent_layout_, recent, style_.recent,
              /*recent=*/true, style_.recent_count, QString(), recent_signature_);
}

void Sidebar::FillSection(QWidget* heading, QVBoxLayout* layout,
                          const std::vector<const GameSummary*>& games, sidebar::Style style,
                          bool recent, int places, const QString& empty_text, QString& signature) {
  // Most refreshes (every game.updated) change nothing shown here; rebuilding
  // anyway makes the rows flicker.
  // A shelf cover is too narrow for "Yesterday": it gets "1d ago".
  const bool shelf = style == sidebar::Style::Shelf;
  const auto trailing = [recent, shelf, this](const GameSummary& game) {
    if (game.running) return QString(RunningLabel(game));
    if (!recent || !style_.recent_when) return QString();
    return shelf ? FormatPlayedAgoShort(game.last_played_at) : FormatPlayedAgo(game.last_played_at);
  };
  const int placeholders = std::max(0, places - static_cast<int>(games.size()));
  QString wanted =
      theme::Current().running.name() + sidebar::StyleKey(style) + QString::number(placeholders);
  for (const GameSummary* game : games) {
    wanted += QString("\n%1\t%2\t%3\t%4\t%5\t%6")
                  .arg(QString::fromStdString(game->id), QString::fromStdString(game->name),
                       QString::fromStdString(game->status), game->running ? "1" : "0",
                       trailing(*game), sidebar::ArtSignature(*game, style, artwork_));
  }
  if (wanted == signature) return;
  signature = wanted;

  QWidget* parent = heading->parentWidget();
  parent->setUpdatesEnabled(false);
  // deleteLater: a row's own click or menu may be what got us here. Hidden
  // first: a popup's nested event loop would otherwise keep it painted.
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      // The hovered row is going away without a Leave, so its card would stay up.
      if (hover_row_ != nullptr && (row == hover_row_ || row->isAncestorOf(hover_row_))) {
        if (hover_timer_ != nullptr) hover_timer_->stop();
        hover_row_ = nullptr;
        emit HoverEnded();
      }
      row->hide();
      row->deleteLater();
    }
    delete item;
  }
  // Empty places up to `places` show as faded sketches, so the section keeps its size.
  heading->setVisible(!games.empty() || placeholders > 0 || !empty_text.isEmpty());
  if (games.empty() && placeholders == 0 && !empty_text.isEmpty()) {
    QLabel* empty = MakeLabel(parent, empty_text, "muted");
    empty->setContentsMargins(8, 2, 8, 2);
    layout->addWidget(empty);
  } else if (shelf) {
    auto* shelf_widget = new sidebar::Shelf(parent);
    for (const GameSummary* game : games) {
      auto* cover = new sidebar::ShelfCover(*game, artwork_, trailing(*game), shelf_widget);
      WireGame(cover, *game);
      shelf_widget->Add(cover);
    }
    for (int i = 0; i < placeholders; ++i) {
      shelf_widget->Add(
          new sidebar::PlaceholderRow(style, static_cast<int>(games.size()) + i, shelf_widget));
    }
    if (!games.empty() || placeholders > 0) {
      layout->addWidget(shelf_widget);
    } else {
      shelf_widget->deleteLater();
    }
  } else {
    for (const GameSummary* game : games) {
      QPushButton* row = style == sidebar::Style::Hero
                             ? new sidebar::HeroRow(*game, artwork_, trailing(*game), parent)
                             : sidebar::MakeCoverRow(*game, artwork_, trailing(*game), parent);
      WireGame(row, *game);
      layout->addWidget(row);
    }
    for (int i = 0; i < placeholders; ++i) {
      layout->addWidget(
          new sidebar::PlaceholderRow(style, static_cast<int>(games.size()) + i, parent));
    }
  }
  parent->setUpdatesEnabled(true);
}

void Sidebar::SetSelectedGames(const QSet<QString>& ids) {
  if (ids == selected_games_) return;
  selected_games_ = ids;
  for (QPushButton* row : findChildren<QPushButton*>()) {
    const QVariant id = row->property("hover_game");
    if (!id.isValid()) continue;
    const bool selected = ids.contains(id.toString());
    if (row->property("selected").toBool() == selected) continue;
    row->setProperty("selected", selected);
    row->style()->unpolish(row);  // a cover row's background comes from its style sheet
    row->style()->polish(row);
    row->update();
  }
}

void Sidebar::WireGame(QPushButton* row, const GameSummary& game) {
  const std::string id = game.id;
  if (!game.running && game.status == "ready") {
    connect(row, &QPushButton::clicked, this, [this, id] { emit PlayRequested(id); });
  }
  row->setProperty("hover_game", QString::fromStdString(id));
  row->setProperty("selected", selected_games_.contains(QString::fromStdString(id)));
  row->installEventFilter(this);
  row->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(row, &QWidget::customContextMenuRequested, this, [this, row, id](const QPoint& pos) {
    emit GameMenuRequested(id, row->mapToGlobal(pos));
  });
}

}  // namespace mira_gui
