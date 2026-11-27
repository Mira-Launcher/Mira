#include "GameCard.h"

#include <QDateTime>
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
#include "../components/Button.h"
#include "../components/Card.h"
#include "../components/ChangeBar.h"
#include "../components/Labels.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../library/PlayChoices.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "AchievementsPage.h"
#include "ArtPickerPanel.h"
#include "GameEditForm.h"
#include "GamePanel.h"
#include "SavesPage.h"
#include "SavesText.h"
#include "StoreInfoPage.h"

namespace mira_gui {

namespace {
constexpr std::size_t kPrefetchThumbs = 24;
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
    : CardBackdrop(artwork, parent), id_(id), library_(library), artwork_(artwork) {
  Build(nullptr);
}

GameCard::GameCard(const GameSummary& title, GameLibraryModel* library, ArtworkStore* artwork,
                   QWidget* parent)
    : CardBackdrop(artwork, parent),
      id_(title.id),
      title_info_(title),
      library_(library),
      artwork_(artwork) {
  Build(&*title_info_);
}

void GameCard::Build(const GameSummary* title) {
  const GameSummary* game = title != nullptr ? title : library_->Find(id_);

  // The art fills the whole card, behind the panel and the cards.
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

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);  // inside the card's border
  layout->setSpacing(0);
  panel_ = new GamePanel(artwork_, this);
  layout->addWidget(panel_);
  layout->addWidget(MakeDivider(this, Qt::Vertical));
  auto* right = new QWidget(this);
  auto* right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(0, 0, 0, 0);
  stack_ = new QStackedWidget(right);
  right_layout->addWidget(stack_);
  layout->addWidget(right, 1);

  // Close at the top right, over the cards' corner, as dark glass in every theme.
  close_ = new Button(QString(), ButtonKind::Glass, this);
  close_->setIcon(icons::For(icons::Glyph::Close, QColor(0xe6, 0xe8, 0xec)));
  close_->setToolTip("Close");
  connect(close_, &QPushButton::clicked, this, &GameCard::RequestClose);

  title_ = panel_->Title();
  status_ = panel_->Status();
  cover_ = panel_->Cover();
  play_ = panel_->Play();
  tools_ = panel_->Tools();
  art_button_ = panel_->ArtButton();
  achievements_button_ = panel_->AchievementsButton();
  title_->setText("Game settings");
  if (game != nullptr) cover_->ShowGame(*game);
  // setChecked is OpenArtPicker/CloseArtPicker's, not the click's.
  connect(art_button_, &QPushButton::clicked, this, [this] {
    art_button_->setChecked(!art_button_->isChecked());
    if (ArtPickerOpen()) {
      CloseArtPicker();
    } else {
      OpenArtPicker();
    }
  });
  // setChecked is OpenStoreInfo/CloseStoreInfo's, as with the art picker.
  connect(panel_->DetailsButton(), &QPushButton::clicked, this, [this] {
    panel_->DetailsButton()->setChecked(!panel_->DetailsButton()->isChecked());
    if (StoreInfoOpen()) {
      CloseStoreInfo();
    } else {
      OpenStoreInfo();
    }
  });
  connect(achievements_button_, &QPushButton::clicked, this, &GameCard::OpenAchievements);
  connect(panel_, &GamePanel::SavesClicked, this, &GameCard::OpenSaves);
  connect(panel_, &GamePanel::BackClicked, this, &GameCard::Back);
  UpdateIdentity();
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
            // Settings' values show in the rows the game takes from Settings.
            if (live && form_ != nullptr && type == "config.changed") form_->Reload();
            if (live && form_ != nullptr &&
                ((type == "game.saves_changed" && events::ParseGameId(data) == id_) ||
                 type == "saves.manifest_changed"))
              LoadSaves();
            // Its store info is fetched again when what it's found by changes.
            if (live && store_info_ != nullptr && type == "game.metadata_ready" &&
                events::ParseGameId(data) == id_)
              store_info_->Load();
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

  bar_ = new ChangeBar(right);
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
    panel_->SetTitleOnly();
    OpenArtPicker();
    return;
  }
  form_ = new GameEditForm(id_, stack_);
  stack_->addWidget(form_);
  connect(form_, &GameEditForm::CardsChanged, this,
          [this] { panel_->SetJumps(form_->Jumps(), form_->Scroll()); });
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
    notify::Failed(window(), QString("Could not load this %1").arg(Noun()), error);
    if (first) emit CloseRequested();
  });
  connect(form_, &GameEditForm::SaveFinished, this, [this](bool ok, QString error) {
    const bool close = std::exchange(close_after_save_, false);
    saving_ = false;
    bar_->SetBusy(false);
    if (!ok) {
      notify::Failed(window(), QString("Could not save this %1").arg(Noun()), error);
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
  if (!form_->Save()) {  // called off: nothing is coming back
    saving_ = close_after_save_ = false;
    bar_->SetBusy(false);
  }
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
  } else if (StoreInfoOpen()) {
    CloseStoreInfo();
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
      notify::LeaveUnsaved(window(), QString("This %1's edits aren't saved").arg(Noun()), [this] {
        close_after_save_ = true;
        Save();
      })) {
    emit CloseRequested();
  }
}

void GameCard::SetLayout(int panel_width, int column_width, bool cover) {
  panel_->setFixedWidth(panel_width);
  panel_->SetCoverShown(cover);
  column_width_ = column_width;
  if (form_ != nullptr) form_->SetColumnWidth(column_width);
  if (store_info_ != nullptr) store_info_->SetColumnWidth(column_width);
}

void GameCard::resizeEvent(QResizeEvent* event) {
  CardBackdrop::resizeEvent(event);
  close_->adjustSize();
  close_->move(width() - close_->width() - 16, 14);
  close_->raise();
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
    connect(picker_, &ArtPickerPanel::PickChanged, this, [this] {
      if (!ArtPickerOpen()) return;
      const auto noun = [](const std::string& slot) -> QString {
        return slot == "hero"   ? "hero"
               : slot == "logo" ? "logo"
               : slot == "icon" ? "icon"
                                : "cover";
      };
      const auto what = [&noun](const std::string& slot) {
        return slot == "hero" ? QString("hero art") : noun(slot);
      };
      const std::vector<std::pair<std::string, bool>> pending = picker_->PendingSlots();
      if (pending.empty()) {
        bar_->SetText(QString(), "Use this " + noun(picker_->slot()), "Cancel");
      } else if (pending.size() == 1) {
        const auto& [slot, none] = pending.front();
        if (none) {
          bar_->SetText(QString("No %1 picked").arg(what(slot)), "Use no " + what(slot), "Cancel");
        } else {
          bar_->SetText(QString("New %1 picked").arg(what(slot)), "Use this " + noun(slot),
                        "Cancel");
        }
      } else {
        // Picks on several tabs: "New cover, hero art and logo picked".
        QStringList names;
        for (const auto& [slot, none] : pending) names << what(slot);
        const QString last = names.takeLast();
        bar_->SetText(QString("New %1 and %2 picked").arg(names.join(", "), last), "Use these",
                      "Cancel");
      }
    });
    connect(picker_, &ArtPickerPanel::PickActivated, this, [this] {
      if (ArtPickerOpen()) picker_->Apply();
    });
    connect(picker_, &ArtPickerPanel::ApplyFailed, this,
            [this](const QString& failed_slot, const QString& error) {
              SetPreview(failed_slot, QPixmap());
              if (failed_slot == "cover") cover_->SetPreview(QPixmap());
              notify::Failed(window(), "Could not change the art", error);
            });
  }
  art_button_->setChecked(true);
  bar_->SetText(QString());
  panel_->DetailsButton()->setChecked(false);
  stack_->setCurrentWidget(picker_);
  panel_->ShowPage("Change art", picker_->Controls());
  picker_->Open("cover");
}

void GameCard::CloseArtPicker(bool applied) {
  if (!ArtPickerOpen()) return;
  // An applied pick stays on screen until its art arrives in its place.
  if (!applied) picker_->ResetPick();  // every tab's pick, and its preview
  art_button_->setChecked(false);
  stack_->setCurrentIndex(0);
  panel_->ShowJumps();
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
  play_->SetKind(running ? ButtonKind::Danger : ButtonKind::Primary);

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
    tools_->addWidget(button, 1);
  }
}

void GameCard::UpdateIdentity() {
  if (title_info_) {
    title_->setText(QString::fromStdString(title_info_->name));
    const SourceInfo* info = FindSourceInfo(QString::fromStdString(title_info_->source));
    status_->setText(
        QString("<span style='color:%1; font-weight:600;'>Not installed</span> · %2")
            .arg(theme::Current().text_muted.name(),
                 (info != nullptr ? info->name : QString::fromStdString(title_info_->source))
                     .toHtmlEscaped()));
    return;
  }
  const GameSummary* game = library_->Find(id_);
  status_->setVisible(game != nullptr);
  panel_->Played()->setVisible(false);
  if (game == nullptr) return;
  title_->setText(QString::fromStdString(game->name));
  const bool running = game->running;
  const QColor status_color = StatusColor(running ? "running" : game->status);
  const SourceInfo* info = FindSourceInfo(QString::fromStdString(game->source));
  const QString source = info != nullptr ? info->name : StatusLabel(game->source);
  QStringList facts;
  if (!source.isEmpty()) facts << source;
  if (!game->platform.empty()) facts << PlatformLabel(game->platform);
  const bool played = game->play_seconds > 0 && !IsApp(*game);
  panel_->Played()->setText(played ? FormatPlaytime(game->play_seconds) + " played" : QString());
  panel_->Played()->setVisible(played);
  facts.prepend(QString());  // the status itself, colored below
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>%3")
                       .arg(status_color.name(),
                            running ? RunningLabel(*game) : StatusLabel(game->status),
                            facts.size() > 1 ? facts.join(" · ").toHtmlEscaped() : QString()));
}

void GameCard::LoadAchievements(bool refresh) {
  const int request = ++achievements_request_;
  api::GetAchievementsAsync(this, id_, [this, refresh, request](AchievementsResult result) {
    if (request != achievements_request_) return;
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
    achievements_page_->Show(*achievements_);
  }
  ShowStackPage(achievements_page_, "Achievements", achievements_page_->Controls());
}

void GameCard::CloseAchievements() {
  if (AchievementsOpen()) ShowForm();
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
      panel_->SetSavesShown(false);
      CloseSaves();
      return;
    }
    saves_ = result;
    panel_->SetSaves(saves_text::Summary(result, QDateTime::currentDateTime()),
                     saves_text::NeedsAttention(result));
    panel_->SetSavesShown(true);
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
    connect(saves_page_, &SavesPage::SettingClicked, this, [this](const std::string& key) {
      CloseSaves();
      form_->RevealSetting(key);
    });
    saves_page_->Show(*saves_);
  }
  ShowStackPage(saves_page_, "Saves", saves_page_->Controls());
}

void GameCard::CloseSaves() {
  if (SavesOpen()) ShowForm();
}

bool GameCard::SavesOpen() const {
  return saves_page_ != nullptr && stack_->currentWidget() == saves_page_;
}

void GameCard::OpenStoreInfo() {
  if (form_ == nullptr) return;
  if (store_info_ == nullptr) {
    store_info_ = new StoreInfoPage(id_, stack_);
    if (column_width_ > 0) store_info_->SetColumnWidth(column_width_);
    stack_->addWidget(store_info_);
  }
  ShowStackPage(store_info_, "Store info", nullptr);
}

void GameCard::CloseStoreInfo() {
  if (StoreInfoOpen()) ShowForm();
}

void GameCard::ShowStackPage(QWidget* page, const QString& title, QWidget* controls) {
  if (ArtPickerOpen()) CloseArtPicker();
  panel_->DetailsButton()->setChecked(page == store_info_);
  stack_->setCurrentWidget(page);
  panel_->ShowPage(title, controls);
}

void GameCard::ShowForm() {
  panel_->DetailsButton()->setChecked(false);
  stack_->setCurrentWidget(form_);
  panel_->ShowJumps();
}

bool GameCard::StoreInfoOpen() const {
  return store_info_ != nullptr && stack_->currentWidget() == store_info_;
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
