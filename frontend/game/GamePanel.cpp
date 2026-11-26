#include "GamePanel.h"

#include <QBoxLayout>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "../components/Button.h"
#include "../components/Card.h"
#include "../components/IconButton.h"
#include "../components/Labels.h"
#include "../components/LaidOutButton.h"
#include "../components/NavRow.h"
#include "../components/Scrolling.h"
#include "../components/SectionNav.h"
#include "../components/Text.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "HeroBackdrop.h"

namespace mira_gui {
namespace {

// The panel's side padding: the cards' inset, as the right side's cards start on it too.
constexpr int kPad = kCardInset + 4;
// A jump row's height, as the sidebar's rows.
constexpr int kJumpRow = 28;

}  // namespace

GamePanel::GamePanel(ArtworkStore* artwork, QWidget* parent) : QWidget(parent) {
  setObjectName("game_panel");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(kPad, 56, kPad, 20);
  layout->setSpacing(12);

  cover_ = new CoverChip(artwork, this);
  cover_->setFixedSize(132, 198);
  layout->addWidget(cover_);

  title_ = new QLabel(this);
  title_->setObjectName("game_edit_title");
  title_->setWordWrap(true);
  layout->addWidget(title_);

  auto* facts = new QVBoxLayout();
  facts->setSpacing(3);
  status_ = new QLabel(this);
  status_->setTextFormat(Qt::RichText);
  status_->setWordWrap(true);
  played_ = new QLabel(this);
  SetTextStyle(played_, text::kDetail);
  facts->addWidget(status_);
  facts->addWidget(played_);
  layout->addLayout(facts);

  play_ = new Button(QString(), ButtonKind::Primary, this);
  play_->setObjectName("game_play");
  layout->addWidget(play_);
  tools_ = new QHBoxLayout();
  tools_->setSpacing(8);
  layout->addLayout(tools_);
  buttons_ = new QBoxLayout(QBoxLayout::LeftToRight);
  auto* buttons = buttons_;
  buttons->setSpacing(8);
  details_ = new QPushButton("Store info…", this);
  art_ = new QPushButton("Change art…", this);
  details_->setCheckable(true);
  art_->setCheckable(true);
  buttons->addWidget(details_, 1);
  buttons->addWidget(art_, 1);
  layout->addLayout(buttons);

  // The saves and achievements lines keep their room while they load, so nothing below moves.
  facts_ = new QWidget(this);
  auto* lines = new QVBoxLayout(facts_);
  lines->setContentsMargins(0, 0, 0, 0);
  lines->setSpacing(2);
  saves_ = new LaidOutButton(facts_);
  saves_->setObjectName("panel_line");
  saves_->setCursor(Qt::PointingHandCursor);
  saves_->setFocusPolicy(Qt::TabFocus);  // a click mustn't leave it lit as focused
  auto* saves_layout = new QGridLayout(saves_);
  saves_layout->setContentsMargins(8, 4, 8, 4);
  saves_layout->setHorizontalSpacing(8);
  saves_layout->setVerticalSpacing(2);
  saves_left_ = new WrapWhenNarrowLabel(QString(), saves_);
  saves_dot_ = new QLabel("·", saves_);
  saves_right_ = new WrapWhenNarrowLabel(QString(), saves_);
  saves_right_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  for (QLabel* label : {saves_left_, saves_dot_, saves_right_})
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
  saves_layout->addWidget(saves_left_, 0, 0);
  saves_layout->addWidget(saves_dot_, 0, 1);
  saves_layout->addWidget(saves_right_, 0, 2);
  saves_layout->setColumnStretch(0, 1);
  saves_layout->setColumnStretch(2, 1);
  connect(saves_, &QPushButton::clicked, this, &GamePanel::SavesClicked);
  saves_->hide();
  lines->addWidget(saves_);
  achievements_ = new QPushButton(facts_);
  achievements_->setObjectName("panel_line");
  achievements_->setCursor(Qt::PointingHandCursor);
  achievements_->setFocusPolicy(Qt::TabFocus);
  icons::Follow(achievements_, icons::Glyph::Trophy, &theme::Tokens::text_muted);
  achievements_->hide();
  lines->addWidget(achievements_);
  saves_->ensurePolished();
  facts_->setMinimumHeight(saves_->sizeHint().height());
  layout->addWidget(facts_);

  layout->addWidget(MakeDivider(this, Qt::Horizontal));

  lower_ = new QStackedWidget(this);
  layout->addWidget(lower_, 1);

  auto* jumps_scroll = new QScrollArea(lower_);
  jumps_scroll->setWidgetResizable(true);
  jumps_scroll->setFrameShape(QFrame::NoFrame);
  jumps_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SeeThrough(jumps_scroll);
  jumps_page_ = new QWidget();
  jumps_layout_ = new QVBoxLayout(jumps_page_);
  jumps_layout_->setContentsMargins(0, 0, 0, 0);
  jumps_layout_->setSpacing(1);
  jumps_layout_->addStretch(1);
  jumps_scroll->setWidget(jumps_page_);
  lower_->addWidget(jumps_scroll);

  page_ = new QWidget(lower_);
  page_layout_ = new QVBoxLayout(page_);
  page_layout_->setContentsMargins(0, 0, 0, 0);
  page_layout_->setSpacing(10);
  auto* header = new QHBoxLayout();
  header->setSpacing(8);
  auto* back = new IconButton(icons::Glyph::ArrowLeft, "Back", page_);
  connect(back, &QToolButton::clicked, this, &GamePanel::BackClicked);
  page_title_ = new QLabel(page_);
  SetTextStyle(page_title_, text::kHeading);
  header->addWidget(back);
  header->addWidget(page_title_, 1);
  page_layout_->addLayout(header);
  page_layout_->addStretch(1);
  lower_->addWidget(page_);
}

void GamePanel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  // Store info and Change art side by side while both labels fit, else one above the other.
  const int room = width() - 2 * kPad;
  const bool fit =
      details_->sizeHint().width() + art_->sizeHint().width() + buttons_->spacing() <= room;
  buttons_->setDirection(fit ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
  FitSavesLine();
}

void GamePanel::FitSavesLine() {
  auto* grid = static_cast<QGridLayout*>(saves_->layout());
  const QMargins margins = grid->contentsMargins();
  const int room = width() - 2 * kPad - margins.left() - margins.right();
  const auto natural = [](const QLabel* label) {
    return label->fontMetrics().horizontalAdvance(label->text()) + 1;
  };
  for (QLabel* label : {saves_left_, saves_right_}) {
    label->setMinimumWidth(0);
    label->setMaximumWidth(QWIDGETSIZE_MAX);
  }
  const bool two = !saves_right_->text().isEmpty();
  const int parts = room - saves_dot_->sizeHint().width() - 2 * grid->horizontalSpacing();
  const int left = natural(saves_left_);
  const int right = natural(saves_right_);
  const bool one_line = !two || left + right <= parts;
  // One line with the dot between while both parts fit, centered when each fits its half;
  // otherwise the second part goes under the first rather than being cut short.
  grid->removeWidget(saves_right_);
  if (one_line)
    grid->addWidget(saves_right_, 0, 2);
  else
    grid->addWidget(saves_right_, 1, 0, 1, 3);
  saves_right_->setAlignment((one_line ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
  saves_right_->setVisible(two);
  saves_dot_->setVisible(two && one_line);
  grid->setColumnStretch(2, two && one_line ? 1 : 0);
  if (two && one_line && (left > parts / 2 || right > parts / 2)) {
    saves_left_->setFixedWidth(left);
    saves_right_->setFixedWidth(parts - left);
  }
}

void GamePanel::SetSaves(const QString& text, bool attention) {
  const qsizetype cut = text.indexOf(" · ");
  saves_left_->setText(cut < 0 ? text : text.left(cut));
  saves_right_->setText(cut < 0 ? QString() : text.mid(cut + 3));
  theme::SetStyleProperty(saves_, "attention", attention ? "true" : "false");
  saves_->setToolTip(text);
  FitSavesLine();
}

void GamePanel::SetSavesShown(bool shown) {
  saves_->setVisible(shown);
}

void GamePanel::SetTitleOnly() {
  play_->hide();
  details_->hide();
  art_->hide();
  facts_->hide();
}

void GamePanel::SetCoverShown(bool shown) {
  cover_->setVisible(shown);
}

void GamePanel::SetJumps(const std::vector<GameEditForm::JumpGroup>& groups, QScrollArea* scroll) {
  std::vector<QWidget*> cards;
  for (const auto& group : groups)
    cards.insert(cards.end(), group.cards.begin(), group.cards.end());
  if (cards == jump_cards_) return SyncJumps();
  jump_cards_ = cards;

  for (const JumpRow& row : jump_rows_) row.button->deleteLater();
  for (QLabel* heading : jump_headings_) heading->deleteLater();
  jump_rows_.clear();
  jump_headings_.clear();
  delete jump_nav_;
  jump_nav_ = new SectionNav(scroll, kCardInset, this);
  int at = 0;
  for (const auto& group : groups) {
    auto* heading = MakeGroupHeading(jumps_page_, group.title);
    heading->setContentsMargins(0, at == 0 ? 0 : 8, 0, 3);
    jumps_layout_->insertWidget(at++, heading);
    jump_headings_.push_back(heading);
    for (Card* card : group.cards) {
      auto* button = new NavRow(card->Title(), jumps_page_);
      button->setFixedHeight(kJumpRow);
      button->setCursor(Qt::PointingHandCursor);
      QLabel* note = button->Note();
      SetTextStyle(note, text::kSubtle);
      jumps_layout_->insertWidget(at++, button);
      jump_nav_->AddSection(button, card);
      jump_rows_.push_back({button, note, card});
      card->installEventFilter(this);
    }
  }
  SyncJumps();
}

bool GamePanel::eventFilter(QObject* watched, QEvent* event) {
  if ((event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) &&
      std::ranges::contains(jump_cards_, watched))
    SyncJumps();
  return QWidget::eventFilter(watched, event);
}

void GamePanel::SyncJumps() {
  for (const JumpRow& row : jump_rows_) {
    row.button->setVisible(!row.card->isHidden());
    const int own = row.card->property("own_count").toInt();
    row.note->setText(own > 0 ? QString("%1 set").arg(own) : QString());
  }
  // A heading with no card showing under it goes too.
  size_t row = 0;
  for (size_t i = 0; i < jump_headings_.size(); ++i) {
    bool any = false;
    const int next = i + 1 < jump_headings_.size() ? jumps_layout_->indexOf(jump_headings_[i + 1])
                                                   : jumps_layout_->count();
    for (; row < jump_rows_.size() && jumps_layout_->indexOf(jump_rows_[row].button) < next; ++row)
      any = any || !jump_rows_[row].card->isHidden();
    jump_headings_[i]->setVisible(any);
  }
  if (jump_nav_ != nullptr) jump_nav_->Sync();
}

void GamePanel::ShowJumps() {
  lower_->setCurrentIndex(0);
}

void GamePanel::ShowPage(const QString& title, QWidget* controls) {
  page_title_->setText(title);
  if (page_controls_ != controls) {
    if (page_controls_ != nullptr) page_controls_->hide();
    if (controls != nullptr) {
      if (page_layout_->indexOf(controls) < 0) page_layout_->insertWidget(1, controls);
      controls->show();
    }
    page_controls_ = controls;
  }
  lower_->setCurrentWidget(page_);
}

}  // namespace mira_gui
