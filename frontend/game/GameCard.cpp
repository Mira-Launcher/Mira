#include "GameCard.h"

#include <QDateTime>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Artwork.h"
#include "../client/api/Games.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../library/PlayChoices.h"
#include "../settings/SettingsCard.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Scrolling.h"
#include "AchievementsPage.h"
#include "ArtPickerPanel.h"
#include "GameEditForm.h"
#include "SavesPage.h"
#include "SavesText.h"

namespace mira_gui {

namespace {
constexpr std::size_t kPrefetchThumbs = 24;
// The summary chips' icons: light on their dark glass in every theme (base.qss).
constexpr const char* kChipIcon = "#c4c9d2";
constexpr const char* kChipWarning = "#f5c46b";

// The saves chip's stack, or a warning sign while a backup failed.
QIcon SavesIcon(bool attention) {
  return icons::For(attention ? icons::Glyph::Warning : icons::Glyph::Layers,
                    QColor(attention ? kChipWarning : kChipIcon));
}

// "48 of 52 achievements", or which store has none for the game.
QString AchievementsSummary(const AchievementsResult& result) {
  if (result.total > 0)
    return QString("%1 of %2 achievements").arg(result.unlocked).arg(result.total);
  const SourceInfo* store = FindSourceInfo(QString::fromStdString(result.source));
  return store != nullptr ? "No achievements on " + store->name : QString("No achievements");
}
}  // namespace

GameCard::GameCard(const std::string& id, GameLibraryModel* library, ArtworkStore* artwork,
                   QWidget* parent)
    : HeroBackdrop(artwork, parent), id_(id), library_(library), artwork_(artwork) {
  Build(nullptr);
}

GameCard::GameCard(const GameSummary& title, GameLibraryModel* library, ArtworkStore* artwork,
                   QWidget* parent)
    : HeroBackdrop(artwork, parent),
      id_(title.id),
      title_info_(title),
      library_(library),
      artwork_(artwork) {
  Build(&*title_info_);
}

void GameCard::Build(const GameSummary* title) {
  const theme::Tokens& tokens = theme::Current();
  const GameSummary* game = title != nullptr ? title : library_->Find(id_);

  // The hero fills the card's top and fades into it; everything below sits
  // over it.
  if (game != nullptr) ShowGame(*game);

  // The first covers' previews, so Change art opens on pictures rather than placeholders.
  // Dropped when the card closes.
  api::GetMetadataAsync(this, id_, [this](GameMetadataResult result) {
    if (!result.ok || result.missing) return;
    std::vector<std::int64_t> ids;
    for (const ArtCandidate& candidate : result.metadata.candidates["cover"]) {
      if (ids.size() == kPrefetchThumbs) break;
      ids.push_back(candidate.id);
    }
    if (!ids.empty()) api::FetchArtThumbsAsync(this, id_, "cover", ids, [](GameActionResult) {});
  });

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // Back at the top left, as in Settings; the art's one button at the right.
  auto* top = new QHBoxLayout();
  top->setContentsMargins(14, 12, 14, 0);
  // hero_action's text color in base.qss: the buttons are dark glass in every theme.
  const QColor on_glass(0xe6, 0xe8, 0xec);
  auto* back = new QPushButton(this);
  back->setObjectName("hero_action");
  back->setIcon(icons::For(icons::Glyph::ArrowLeft, on_glass));
  back->setToolTip("Back");
  connect(back, &QPushButton::clicked, this, &GameCard::Back);
  art_button_ = new QPushButton("Change art", this);
  art_button_->setObjectName("hero_action");
  art_button_->setIcon(icons::For(icons::Glyph::Image, on_glass));
  art_button_->setCheckable(true);
  // setChecked is OpenArtPicker/CloseArtPicker's, not the click's.
  connect(art_button_, &QPushButton::clicked, this, [this] {
    art_button_->setChecked(!art_button_->isChecked());
    if (ArtPickerOpen()) {
      CloseArtPicker();
    } else {
      OpenArtPicker();
    }
  });
  top->addWidget(back);
  top->addStretch(1);
  top->addWidget(art_button_);
  // A title's card is only the picker: Back closes it.
  art_button_->setVisible(title == nullptr);
  layout->addLayout(top);

  // Name and status over the art. The shadow, in the surface's color, keeps
  // it off the art's detail.
  auto* header = new QHBoxLayout();
  header->setContentsMargins(24, 4, 20, 16);
  header->setSpacing(16);
  auto* identity = new QVBoxLayout();
  identity->setSpacing(4);
  title_ = new QLabel("Game settings", this);
  title_->setObjectName("game_edit_title");
  title_->setWordWrap(true);
  auto* shadow = new QGraphicsDropShadowEffect(title_);
  shadow->setColor(tokens.surface);
  shadow->setBlurRadius(18);
  shadow->setOffset(0, 1);
  title_->setGraphicsEffect(shadow);
  identity->addWidget(title_);
  status_ = new QLabel(this);
  status_->setTextFormat(Qt::RichText);
  identity->addWidget(status_);
  achievements_button_ = new QPushButton(this);
  achievements_button_->setObjectName("achievements_summary");
  achievements_button_->setCursor(Qt::PointingHandCursor);
  // A click mustn't leave it lit as focused; Tab still reaches it.
  achievements_button_->setFocusPolicy(Qt::TabFocus);
  // Light on dark glass in every theme, as base.qss draws the chips.
  achievements_button_->setIcon(icons::For(icons::Glyph::Trophy, QColor(kChipIcon)));
  connect(achievements_button_, &QPushButton::clicked, this, &GameCard::OpenAchievements);
  achievements_button_->hide();
  saves_button_ = new QPushButton(this);
  saves_button_->setObjectName("saves_summary");
  saves_button_->setCursor(Qt::PointingHandCursor);
  saves_button_->setFocusPolicy(Qt::TabFocus);
  saves_button_->setIcon(SavesIcon(/*attention=*/false));
  connect(saves_button_, &QPushButton::clicked, this, &GameCard::OpenSaves);
  saves_button_->hide();
  // Only a game in the library has summary lines; a title's card is the art picker. The row keeps
  // its height while they load, so the name above doesn't move.
  if (title == nullptr) {
    auto* facts_box = new QWidget(this);
    auto* facts = new QHBoxLayout(facts_box);
    facts->setContentsMargins(0, 0, 0, 0);
    facts->setSpacing(8);
    facts->addWidget(achievements_button_);
    facts->addWidget(saves_button_);
    facts->addStretch(1);
    achievements_button_->ensurePolished();
    facts_box->setMinimumHeight(achievements_button_->sizeHint().height());
    identity->addWidget(facts_box);
  }
  UpdateIdentity();
  // The cover, which the hero otherwise hides, and where a picked one previews.
  cover_ = new CoverChip(artwork_, this);
  if (game != nullptr) cover_->ShowGame(*game);
  identity->insertStretch(0, 1);
  play_ = new QPushButton(this);
  play_->setObjectName("game_play");
  connect(play_, &QPushButton::clicked, this, &GameCard::PlayClicked);
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &GameCard::UpdatePlay);
  // Follows the game starting and stopping, and changes made elsewhere (the CLI, a batch edit).
  connect(library_, &QAbstractItemModel::dataChanged, this, &GameCard::UpdatePlay);
  connect(library_, &QAbstractItemModel::modelReset, this, &GameCard::UpdatePlay);
  connect(library_, &QAbstractItemModel::dataChanged, this, &GameCard::UpdateIdentity);
  connect(library_, &QAbstractItemModel::modelReset, this, &GameCard::UpdateIdentity);
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live && form_ != nullptr && ChangesThisGame(type, data)) {
              form_->Reload();
              LoadSaves();  // its saves settings may be what changed
            }
            if (live && form_ != nullptr &&
                ((type == "game.saves_changed" && events::ParseGameId(data) == id_) ||
                 type == "saves.manifest_changed"))
              LoadSaves();
            if (live && saves_page_ != nullptr && type == "game.saves_activity" &&
                events::ParseGameId(data) == id_)
              saves_page_->SetDoing(events::ParseSavesDoing(data));
            if (live && form_ != nullptr && type == "config.changed" &&
                std::ranges::any_of(events::ParseChangedKeys(data), [](const std::string& key) {
                  return key.starts_with("saves.");
                }))
              LoadSaves();
            // After a session, or a refresh asked for elsewhere.
            if (live && form_ != nullptr && type == "game.achievements_ready" &&
                events::ParseGameId(data) == id_)
              LoadAchievements(/*refresh=*/false);
            if (live && form_ != nullptr && type == "config.changed" &&
                std::ranges::contains(events::ParseChangedKeys(data), "achievements.enabled"))
              LoadAchievements(/*refresh=*/true);
          });
  header->addWidget(cover_, 0, Qt::AlignBottom);
  header->addLayout(identity, /*stretch=*/1);
  tools_ = new QHBoxLayout();
  tools_->setSpacing(8);
  header->addLayout(tools_);
  header->addWidget(play_, 0, Qt::AlignBottom);
  layout->addLayout(header);

  // Translucent, so the art still shows through at its top edge.
  auto* panel = new QWidget(this);
  panel->setObjectName("game_edit_panel");
  panel->setAttribute(Qt::WA_StyledBackground);
  const auto style_panel = [panel] {
    const theme::Tokens& current = theme::Current();
    QColor panel_color = current.window;
    panel_color.setAlphaF(0.82);
    panel->setStyleSheet(QString("QWidget#game_edit_panel { background: %1; border: 1px solid "
                                 "%2; border-radius: %3px; }")
                             .arg(theme::ColorToQss(panel_color))
                             .arg(current.border.name())
                             .arg(current.radius_panel));
  };
  style_panel();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, panel, style_panel);
  auto* panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(0, 0, 0, 0);
  auto* panel_row = new QHBoxLayout();
  panel_row->setContentsMargins(20, 0, 20, 20);
  panel_row->addWidget(panel);
  layout->addLayout(panel_row, /*stretch=*/1);

  stack_ = new QStackedWidget(panel);
  panel_layout->addWidget(stack_);
  bar_ = new ChangeBar(this);
  // While the art picker is open the bar is its Cancel and Use.
  connect(bar_, &ChangeBar::DiscardClicked, this, [this] {
    if (ArtPickerOpen()) return picker_->ResetPick();
    form_->DiscardChanges();
  });
  connect(bar_, &ChangeBar::SaveClicked, this, [this] {
    // The picker stays open, to change another slot or pick again.
    if (ArtPickerOpen()) return picker_->Apply();
    Save();
  });
  if (title != nullptr) {
    play_->hide();
    OpenArtPicker();
    return;
  }
  form_ = new GameEditForm(id_, stack_);
  stack_->addWidget(form_);
  UpdatePlay();
  QStringList tags;
  for (const GameSummary& known : library_->Games()) {
    for (const std::string& tag : known.tags) tags << QString::fromStdString(tag);
  }
  tags.removeDuplicates();
  tags.sort(Qt::CaseInsensitive);
  form_->SetTagSuggestions(tags);
  form_->SetTagOrder(TagOrder(library_->Games()));
  LoadAchievements(/*refresh=*/true);
  LoadSaves();

  connect(form_, &GameEditForm::Changed, this, [this] {
    if (!ArtPickerOpen()) UpdateBar();
  });
  connect(form_, &GameEditForm::Loaded, title_, &QLabel::setText);
  connect(form_, &GameEditForm::TagFilterRequested, this, &GameCard::TagFilterRequested);
  connect(form_, &GameEditForm::LoadFailed, this, [this](QString error, bool first) {
    notify::Failed(window(), QString("Could not load this %1.").arg(Noun()), error);
    if (first) emit CloseRequested();
  });
  connect(form_, &GameEditForm::SaveFinished, this, [this](bool ok, QString error) {
    const bool close = std::exchange(close_after_save_, false);
    saving_ = false;
    bar_->SetBusy(false);
    if (!ok) {
      notify::Failed(window(), QString("Could not save this %1.").arg(Noun()), error);
      return;
    }
    // The change bar going away is the feedback; no notice for a save the user just made.
    emit Saved();
    if (close) emit CloseRequested();
  });
}

bool GameCard::IsDirty() const {
  return form_ != nullptr && form_->IsDirty();
}

void GameCard::Save() {
  if (saving_ || form_ == nullptr) return;
  saving_ = true;
  bar_->SetBusy(true);
  form_->Save();
}

void GameCard::Back() {
  if (form_ == nullptr) {
    RequestClose();
  } else if (ArtPickerOpen()) {
    CloseArtPicker();
  } else if (AchievementsOpen()) {
    CloseAchievements();
  } else if (SavesOpen()) {
    CloseSaves();
  } else if (form_->AdvancedOpen()) {
    form_->CloseAdvanced();
  } else {
    RequestClose();
  }
}

void GameCard::RequestClose() {
  // Mid-save it closes once the save is in, rather than tear down what's still being written.
  if (saving_) {
    close_after_save_ = true;
    return;
  }
  if (!IsDirty() ||
      notify::LeaveUnsaved(window(), QString("This %1's edits aren't saved.").arg(Noun()), [this] {
        close_after_save_ = true;
        Save();
      })) {
    emit CloseRequested();
  }
}

void GameCard::UpdateCover(const std::string& id) {
  // The card draws the same game at another size and can't notice the store
  // changing. A no-op for another game.
  RefreshCover(id);
  cover_->RefreshCover(id);
}

bool GameCard::ArtPickerOpen() const {
  return picker_ != nullptr && stack_->currentWidget() == picker_;
}

void GameCard::OpenArtPicker() {
  if (ArtPickerOpen()) return;
  if (picker_ == nullptr) {
    picker_ = new ArtPickerPanel(id_, stack_, /*title=*/form_ == nullptr);
    stack_->addWidget(picker_);
    connect(picker_, &ArtPickerPanel::Previewed, this,
            [this](const QString& preview_slot, const QPixmap& preview) {
              SetPreview(preview_slot, preview);
              if (preview_slot == "cover") cover_->SetPreview(preview);
            });
    // The change bar is the picker's only while it's open; an apply can land after.
    connect(picker_, &ArtPickerPanel::PickChanged, this, [this](bool has_change) {
      if (!ArtPickerOpen()) return;
      const std::string& slot = picker_->slot();
      const QString noun = slot == "hero"   ? "hero"
                           : slot == "logo" ? "logo"
                           : slot == "icon" ? "icon"
                                            : "cover";
      const QString what = slot == "hero" ? "hero art" : noun;
      if (has_change && picker_->PicksNone()) {
        bar_->SetText(QString("No %1 picked").arg(what), "Use no " + what, "Cancel");
        return;
      }
      bar_->SetText(has_change ? QString("New %1 picked").arg(what) : QString(), "Use this " + noun,
                    "Cancel");
    });
    connect(picker_, &ArtPickerPanel::PickActivated, this, [this] {
      if (ArtPickerOpen()) picker_->Apply();
    });
    connect(picker_, &ArtPickerPanel::ApplyFailed, this,
            [this](const QString& failed_slot, const QString& error) {
              SetPreview(failed_slot, QPixmap());
              if (failed_slot == "cover") cover_->SetPreview(QPixmap());
              notify::Failed(window(), "Could not change the art.", error);
            });
  }
  art_button_->setChecked(true);
  bar_->SetText(QString());
  stack_->setCurrentWidget(picker_);
  picker_->Open("cover");
}

void GameCard::CloseArtPicker(bool applied) {
  if (!ArtPickerOpen()) return;
  // An applied pick stays on screen until its art arrives in its place.
  if (!applied) {
    SetPreview(QString::fromStdString(picker_->slot()), QPixmap());
    cover_->SetPreview(QPixmap());
  }
  art_button_->setChecked(false);
  stack_->setCurrentIndex(0);
  UpdateBar();
}

void GameCard::UpdateBar() {
  if (form_ == nullptr) return;
  bar_->SetCount(form_->ChangeCount());
  if (!bar_->isHidden() && bar_->RoomNeeded() > bottom_room_) {
    bottom_room_ = bar_->RoomNeeded();
    form_->SetBottomRoom(bottom_room_);
  }
}

QString GameCard::Noun() const {
  const GameSummary* game = library_->Find(id_);
  return game != nullptr ? mira_gui::Noun(*game) : "game";
}

void GameCard::UpdatePlay() {
  const GameSummary* game = library_->Find(id_);
  const bool running = game != nullptr && game->running;
  // A launch would only wait for mirad's saves work, so the button says what that is.
  const QString saves_doing = running ? QString() : SavesDoingLabel(library_->SavesDoing(id_));
  play_->setText(game == nullptr          ? "Play"
                 : running                ? "Stop"
                 : !saves_doing.isEmpty() ? saves_doing
                                          : RunVerb(*game));
  play_->setEnabled(game != nullptr && saves_doing.isEmpty() && CanPlayOrStop(*game));
  // A running game's button is a red Stop, and not the default, so Enter can't end the game.
  play_->setDefault(!running);
  play_->setIcon(running ? icons::For(icons::Glyph::Stop, theme::Current().on_accent)
                         : icons::For(icons::Glyph::Play, theme::Current().on_accent));
  if (play_->property("stop").toBool() != running) {
    play_->setProperty("stop", running);
    play_->style()->unpolish(play_);
    play_->style()->polish(play_);
  }

  const std::vector<PlayChoice> tools =
      game != nullptr ? ToolChoices(*game) : std::vector<PlayChoice>();
  std::vector<std::string> ids;
  for (const PlayChoice& tool : tools) ids.push_back(tool.option + '\n' + tool.label.toStdString());
  if (ids == tool_ids_) return;
  tool_ids_ = std::move(ids);
  while (QLayoutItem* item = tools_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (const PlayChoice& tool : tools) {
    auto* button = new QPushButton(tool.label, this);
    button->setToolTip("Open " + tool.label + " for this " + Noun());
    connect(button, &QPushButton::clicked, this,
            [this, option = tool.option] { actions::Launch(this, id_, option, {}); });
    tools_->addWidget(button, 0, Qt::AlignBottom);
  }
}

void GameCard::UpdateIdentity() {
  if (title_info_) {
    title_->setText(QString::fromStdString(title_info_->name));
    const SourceInfo* info = FindSourceInfo(QString::fromStdString(title_info_->source));
    status_->setText(
        QString("<span style='color:%1; font-weight:600;'>Not installed</span>&nbsp;&nbsp;%2")
            .arg(theme::Current().text_muted.name(),
                 (info != nullptr ? info->name : QString::fromStdString(title_info_->source))
                     .toHtmlEscaped()));
    return;
  }
  const GameSummary* game = library_->Find(id_);
  status_->setVisible(game != nullptr);
  if (game == nullptr) return;
  title_->setText(QString::fromStdString(game->name));
  const bool running = game->running;
  const QColor status_color = StatusColor(running ? "running" : game->status);
  const SourceInfo* info = FindSourceInfo(QString::fromStdString(game->source));
  const QString source = info != nullptr ? info->name : StatusLabel(game->source);
  QStringList facts;
  if (!source.isEmpty()) facts << source;
  if (!game->platform.empty()) facts << StatusLabel(game->platform);
  if (game->play_seconds > 0 && !IsApp(*game))
    facts << FormatPlaytime(game->play_seconds) + " played";
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>&nbsp;&nbsp;%3")
                       .arg(status_color.name(),
                            running ? RunningLabel(*game) : StatusLabel(game->status),
                            facts.join(" · ").toHtmlEscaped()));
}

void GameCard::LoadAchievements(bool refresh) {
  api::GetAchievementsAsync(this, id_, [this, refresh](AchievementsResult result) {
    // A game no store reads them for, or with them turned off, shows nothing.
    if (!result.ok && !result.missing) {
      achievements_.reset();
      achievements_button_->hide();
      CloseAchievements();
      return;
    }
    if (result.ok) {
      achievements_ = result;
      achievements_button_->setText(AchievementsSummary(result));
      achievements_button_->setEnabled(result.total > 0);
      achievements_button_->setCursor(result.total > 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
      achievements_button_->show();
      if (achievements_page_ != nullptr) achievements_page_->Show(result);
      if (result.total == 0 && AchievementsOpen()) CloseAchievements();
    }
    constexpr std::int64_t kDay = 24 * 60 * 60;
    const bool stale =
        result.missing || QDateTime::currentSecsSinceEpoch() - result.fetched_at > kDay;
    if (!refresh || !stale) return;
    // Quiet: what changed shows when it lands, and a failure leaves what's cached.
    api::RefreshAchievementsAsync(this, id_, [this](AchievementsRefreshResult refreshed) {
      if (refreshed.ok) LoadAchievements(/*refresh=*/false);
    });
  });
}

void GameCard::OpenAchievements() {
  if (!achievements_ || form_ == nullptr) return;
  if (achievements_page_ == nullptr) {
    achievements_page_ = new AchievementsPage(id_, stack_);
    stack_->addWidget(achievements_page_);
    connect(achievements_page_, &AchievementsPage::BackClicked, this, &GameCard::CloseAchievements);
    achievements_page_->Show(*achievements_);
  }
  if (ArtPickerOpen()) CloseArtPicker();
  stack_->setCurrentWidget(achievements_page_);
  FocusPage(achievements_page_);
}

void GameCard::CloseAchievements() {
  if (!AchievementsOpen()) return;
  stack_->setCurrentWidget(form_);
}

bool GameCard::AchievementsOpen() const {
  return achievements_page_ != nullptr && stack_->currentWidget() == achievements_page_;
}

void GameCard::LoadSaves() {
  const int request = ++saves_request_;
  api::GetSavesAsync(this, id_, [this, request](SavesResult result) {
    if (request != saves_request_) return;
    // Activity events arrive in order and may be newer than this answer's `doing`.
    result.doing = library_->SavesDoing(id_);
    if (!result.ok) {
      saves_.reset();
      saves_button_->hide();
      CloseSaves();
      return;
    }
    saves_ = result;
    saves_button_->setText(saves_text::Summary(result, QDateTime::currentDateTime()));
    const bool attention = saves_text::NeedsAttention(result);
    if (saves_button_->property("attention").toBool() != attention) {
      saves_button_->setProperty("attention", attention);
      saves_button_->setIcon(SavesIcon(attention));
      saves_button_->style()->unpolish(saves_button_);
      saves_button_->style()->polish(saves_button_);
    }
    saves_button_->show();
    if (saves_page_ != nullptr) saves_page_->Show(result);
    if (std::exchange(open_saves_when_loaded_, false)) OpenSaves();
  });
}

void GameCard::OpenSaves() {
  if (form_ == nullptr) return;
  if (!saves_) {
    open_saves_when_loaded_ = true;  // asked for by a notification before they arrived
    return;
  }
  if (saves_page_ == nullptr) {
    saves_page_ = new SavesPage(id_, stack_);
    stack_->addWidget(saves_page_);
    connect(saves_page_, &SavesPage::BackClicked, this, &GameCard::CloseSaves);
    connect(saves_page_, &SavesPage::SettingClicked, this, [this](const std::string& key) {
      CloseSaves();
      form_->OpenAdvanced(key);
    });
    saves_page_->Show(*saves_);
  }
  if (ArtPickerOpen()) CloseArtPicker();
  stack_->setCurrentWidget(saves_page_);
  FocusPage(saves_page_);
}

void GameCard::CloseSaves() {
  if (!SavesOpen()) return;
  stack_->setCurrentWidget(form_);
}

bool GameCard::SavesOpen() const {
  return saves_page_ != nullptr && stack_->currentWidget() == saves_page_;
}

bool GameCard::ChangesThisGame(const std::string& type, const std::string& data) const {
  if (type == "game.updated") {
    GameSummary game;
    return events::ParseGameSummary(data, &game) && game.id == id_;
  }
  if (type == "games.updated") {
    std::vector<GameSummary> games;
    return events::ParseGameSummaries(data, &games) &&
           std::ranges::any_of(games, [&](const GameSummary& game) { return game.id == id_; });
  }
  return false;
}

}  // namespace mira_gui
