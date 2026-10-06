#include "ContinueRow.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "ArtworkStore.h"
#include "GamePresentation.h"
#include "../theme/Icons.h"

namespace mira_gui {
namespace {

const QSize kCover(80, 120);

}  // namespace

ContinueRow::ContinueRow(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  auto* heading = new QLabel("Continue playing", this);
  heading->setProperty("role", "section");
  layout->addWidget(heading);
  cards_ = new QHBoxLayout();
  cards_->setSpacing(12);
  layout->addLayout(cards_);
}

void ContinueRow::SetGames(const std::vector<const GameSummary*>& games) {
  if (std::ranges::equal(games, shown_, [](const GameSummary* a, const GameSummary& b) { return *a == b; })) return;
  // deleteLater: a card's own button may be what got us here.
  while (QLayoutItem* item = cards_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
  shown_.clear();
  covers_.clear();
  for (const GameSummary* game : games) {
    shown_.push_back(*game);
    cards_->addWidget(MakeCard(*game, game->running));
  }
  cards_->addStretch(1);  // cards pack to the left
  setVisible(!games.empty());
}

void ContinueRow::RefreshCover(const std::string& id) {
  const auto cover = covers_.find(id);
  if (cover == covers_.end()) return;
  const auto game = std::ranges::find(shown_, id, &GameSummary::id);
  cover->second->setPixmap(artwork_->Cover(*game, kCover, devicePixelRatioF()));
}

QWidget* ContinueRow::MakeCard(const GameSummary& game, bool running) {
  auto* card = new QFrame(this);
  card->setObjectName("continue_card");
  card->setFixedHeight(kCover.height() + 16);
  card->setFixedWidth(340);
  const QString id = QString::fromStdString(game.id);
  card->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(card, &QWidget::customContextMenuRequested, this,
          [this, card, id](const QPoint& pos) { emit MenuRequested(id, card->mapToGlobal(pos)); });

  auto* layout = new QHBoxLayout(card);
  layout->setContentsMargins(8, 8, 12, 8);
  layout->setSpacing(12);
  auto* cover = new QLabel(card);
  cover->setFixedSize(kCover);
  cover->setPixmap(artwork_->Cover(game, kCover, devicePixelRatioF()));
  covers_[game.id] = cover;
  layout->addWidget(cover);

  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  auto* name = new QLabel(QString::fromStdString(game.name), card);
  name->setProperty("role", "section");
  name->setWordWrap(true);
  text->addWidget(name);
  const QString when = running ? QString(RunningLabel(game)) + " now" : FormatPlayedAgo(game.last_played_at);
  auto* meta = new QLabel(IsApp(game) ? when : QString("%1 · %2").arg(when, FormatPlaytime(game.play_seconds)), card);
  meta->setProperty("role", "muted");
  text->addWidget(meta);
  text->addStretch(1);

  auto* play = new QPushButton(running ? "Stop" : RunVerb(game), card);
  play->setIcon(icons::For(icons::Glyph::Play));
  play->setEnabled(CanPlayOrStop(game));
  connect(play, &QPushButton::clicked, this, [this, id] { emit PlayToggled(id); });
  auto* actions = new QHBoxLayout();
  actions->addWidget(play);
  actions->addStretch(1);
  text->addLayout(actions);
  layout->addLayout(text, /*stretch=*/1);
  return card;
}

}  // namespace mira_gui
