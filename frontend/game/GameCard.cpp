#include "GameCard.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../settings/SettingsCard.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "ArtPickerPanel.h"
#include "GameEditForm.h"

namespace mira_gui {

GameCard::GameCard(const std::string& id, GameLibraryModel* library, ArtworkStore* artwork,
                   QWidget* parent)
    : HeroBackdrop(artwork, parent), id_(id), library_(library), artwork_(artwork) {
  const theme::Tokens& tokens = theme::Current();
  const GameSummary* game = library_->Find(id);

  // The hero fills the card's top and fades into it; everything below sits
  // over it.
  if (game != nullptr) ShowGame(*game);

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
  UpdateIdentity();
  // The cover, which the hero otherwise hides, and where a picked one previews.
  cover_ = new CoverChip(artwork_, this);
  if (game != nullptr) cover_->ShowGame(*game);
  identity->insertStretch(0, 1);
  play_ = new QPushButton(this);
  icons::Follow(play_, icons::Glyph::Play, &theme::Tokens::on_accent);
  play_->setDefault(true);
  connect(play_, &QPushButton::clicked, this, &GameCard::PlayClicked);
  // Follows the game starting and stopping, and changes made elsewhere (the CLI, a batch edit).
  connect(library_, &QAbstractItemModel::dataChanged, this, &GameCard::UpdatePlay);
  connect(library_, &QAbstractItemModel::modelReset, this, &GameCard::UpdatePlay);
  connect(library_, &QAbstractItemModel::dataChanged, this, &GameCard::UpdateIdentity);
  connect(library_, &QAbstractItemModel::modelReset, this, &GameCard::UpdateIdentity);
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live && ChangesThisGame(type, data)) form_->Reload();
          });
  header->addWidget(cover_, 0, Qt::AlignBottom);
  header->addLayout(identity, /*stretch=*/1);
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
    panel->setStyleSheet(
        QString("QWidget#game_edit_panel { background: %1; border: 1px solid "
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
  form_ = new GameEditForm(id, stack_);
  stack_->addWidget(form_);
  UpdatePlay();
  QStringList tags;
  for (const GameSummary& known : library_->Games()) {
    for (const std::string& tag : known.tags) tags << QString::fromStdString(tag);
  }
  tags.removeDuplicates();
  tags.sort(Qt::CaseInsensitive);
  form_->SetTagSuggestions(tags);

  bar_ = new ChangeBar(this);
  // While the art picker is open the bar is its Cancel and Use.
  connect(bar_, &ChangeBar::DiscardClicked, this, [this] {
    if (ArtPickerOpen()) return picker_->ResetPick();
    form_->DiscardChanges();
  });
  connect(bar_, &ChangeBar::SaveClicked, this, [this] {
    if (ArtPickerOpen()) {
      picker_->Apply();
      CloseArtPicker(/*applied=*/true);
      return;
    }
    Save();
  });
  connect(form_, &GameEditForm::Changed, this, [this] {
    if (!ArtPickerOpen()) UpdateBar();
  });
  connect(form_, &GameEditForm::Loaded, title_, &QLabel::setText);
  connect(form_, &GameEditForm::LoadFailed, this, [this](QString error) {
    notify::Failed(window(), "Could not load this game.", error);
    emit CloseRequested();
  });
  connect(form_, &GameEditForm::SaveFinished, this, [this](bool ok, QString error) {
    const bool close = std::exchange(close_after_save_, false);
    bar_->SetBusy(false);
    if (!ok) {
      notify::Failed(window(), "Could not save this game.", error);
      return;
    }
    // The change bar going away is the feedback; no notice for a save the user just made.
    emit Saved();
    if (close) emit CloseRequested();
  });
}

bool GameCard::IsDirty() const {
  return form_->IsDirty();
}

void GameCard::Save() {
  bar_->SetBusy(true);
  form_->Save();
}

void GameCard::Back() {
  if (ArtPickerOpen()) {
    CloseArtPicker();
  } else if (form_->AdvancedOpen()) {
    form_->CloseAdvanced();
  } else {
    RequestClose();
  }
}

void GameCard::RequestClose() {
  if (!IsDirty() || notify::LeaveUnsaved(window(), "This game's edits aren't saved.", [this] {
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
    picker_ = new ArtPickerPanel(id_, stack_);
    stack_->addWidget(picker_);
    connect(picker_, &ArtPickerPanel::Previewed, this,
            [this](const QString& preview_slot, const QPixmap& preview) {
              SetPreview(preview_slot, preview);
              if (preview_slot == "cover") cover_->SetPreview(preview);
            });
    // The change bar is the picker's only while it's open; an apply can land after.
    connect(picker_, &ArtPickerPanel::PickChanged, this, [this](bool has_change) {
      if (!ArtPickerOpen()) return;
      const bool hero = picker_->slot() == "hero";
      bar_->SetText(has_change ? (hero ? "New hero art picked" : "New cover picked") : QString(),
                    hero ? "Use this hero" : "Use this cover", "Cancel");
    });
    connect(picker_, &ArtPickerPanel::PickActivated, this, [this] {
      if (!ArtPickerOpen()) return;
      picker_->Apply();
      CloseArtPicker(/*applied=*/true);
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
  bar_->SetCount(form_->ChangeCount());
  form_->SetBottomRoom(bar_->isHidden() ? 0 : bar_->RoomNeeded());
}

void GameCard::UpdatePlay() {
  const GameSummary* game = library_->Find(id_);
  play_->setText(game == nullptr ? "Play" : game->running ? "Stop" : RunVerb(*game));
  play_->setEnabled(game != nullptr && CanPlayOrStop(*game));
}

void GameCard::UpdateIdentity() {
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
  if (game->play_seconds > 0 && !IsApp(*game)) facts << FormatPlaytime(game->play_seconds) + " played";
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>&nbsp;&nbsp;%3")
                       .arg(status_color.name(), running ? RunningLabel(*game) : StatusLabel(game->status),
                            facts.join(" · ").toHtmlEscaped()));
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
