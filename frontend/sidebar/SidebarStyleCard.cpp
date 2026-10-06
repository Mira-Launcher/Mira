#include "SidebarStyleCard.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

#include "../library/ArtworkStore.h"
#include "../library/GamePresentation.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

// A tile previewing one style above its name.
class StyleChoice : public QAbstractButton {
public:
  StyleChoice(const sidebar::StyleOption& option, std::function<std::vector<sidebar::PreviewGame>()> games,
              ArtworkStore* artwork, QWidget* parent)
      : QAbstractButton(parent), option_(option), games_(std::move(games)), artwork_(artwork) {
    setCheckable(true);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(option.label);
    setFixedSize(176, 132);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(isChecked() ? tokens.accent : tokens.border, isChecked() ? 2 : 1));
    painter.setBrush(isChecked() || underMouse() ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(frame, tokens.radius_control + 2, tokens.radius_control + 2);
    if (hasFocus()) {
      painter.setPen(QPen(tokens.accent, 1, Qt::DotLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(frame.adjusted(3, 3, -3, -3), tokens.radius_control, tokens.radius_control);
    }

    sidebar::PaintStylePreview(&painter, QRect(12, 12, width() - 24, 86), option_.style, games_(), artwork_);

    QFont label = font();
    label.setWeight(QFont::DemiBold);
    painter.setFont(label);
    painter.setPen(tokens.text);
    painter.drawText(QRect(12, 106, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, option_.label);
  }

private:
  sidebar::StyleOption option_;
  std::function<std::vector<sidebar::PreviewGame>()> games_;
  ArtworkStore* artwork_;
};

}  // namespace

SidebarStyleChoices::SidebarStyleChoices(const Choices& choices, std::vector<GameSummary> pinned,
                                         std::vector<GameSummary> recent, ArtworkStore* artwork, QObject* parent)
    : QObject(parent), choices_(choices), pinned_(std::move(pinned)), recent_(std::move(recent)), artwork_(artwork) {
  pinned_row_ = MakeStyleRow("Pinned", /*recent=*/false);
  recent_row_ = MakeStyleRow("Recently played", /*recent=*/true);

  // Art that lands while this is open shows in its previews.
  const auto repaint = [this] {
    for (QAbstractButton* tile : pinned_tiles_) tile->update();
    for (QAbstractButton* tile : recent_tiles_) tile->update();
  };
  connect(artwork_, &ArtworkStore::CoverChanged, this, repaint);
  connect(artwork_, &ArtworkStore::SlotArtChanged, this, repaint);

  when_row_ = new SettingRow("Show when games were last played", {});
  when_ = new Switch(when_row_);
  when_->setAccessibleName(when_row_->Label()->text());
  connect(when_, &Switch::toggled, this, [this](bool on) {
    choices_.recent_when = on;
    Edited();
  });
  when_row_->AddControl(when_);

  count_row_ = new SettingRow("Games to show", "Running games show first and count toward this.");
  recent_count_ = new QSpinBox(count_row_);
  recent_count_->setRange(0, 10);
  recent_count_->setSpecialValueText("Off");
  recent_count_->setMinimumWidth(90);
  connect(recent_count_, &QSpinBox::valueChanged, this, [this](int count) {
    choices_.recent_count = count;
    Edited();
  });
  count_row_->AddControl(recent_count_);
  Sync();
}

QList<SettingRow*> SidebarStyleChoices::Rows() const { return {pinned_row_, recent_row_, when_row_, count_row_}; }

void SidebarStyleChoices::SetChoices(const Choices& choices) {
  choices_ = choices;
  Sync();
}

void SidebarStyleChoices::Edited() {
  Sync();
  emit Changed(choices_);
}

void SidebarStyleChoices::Sync() {
  const auto check = [](const std::vector<QAbstractButton*>& tiles, sidebar::Style style) {
    const auto& options = sidebar::StyleOptions();
    for (size_t i = 0; i < tiles.size(); ++i) {
      const QSignalBlocker block(tiles[i]);
      tiles[i]->setChecked(options[i].style == style);
      tiles[i]->update();
    }
  };
  check(pinned_tiles_, choices_.pinned);
  check(recent_tiles_, choices_.recent);
  {
    const QSignalBlocker block_when(when_);
    when_->setChecked(choices_.recent_when);
    const QSignalBlocker block_count(recent_count_);
    recent_count_->setValue(choices_.recent_count);
  }
}

std::vector<sidebar::PreviewGame> SidebarStyleChoices::PreviewGames(bool recent, sidebar::Style style) const {
  std::vector<sidebar::PreviewGame> games;
  // Running games always show, the rest up to the count; on a shelf the running ones are part of it.
  int played = 0;
  if (recent && style == sidebar::Style::Shelf) {
    played = static_cast<int>(std::ranges::count(recent_, true, &GameSummary::running));
  }
  for (const GameSummary& game : recent ? recent_ : pinned_) {
    QString trailing;
    QString short_trailing;
    if (game.running) {
      trailing = short_trailing = "Playing";
    } else if (recent) {
      if (played++ >= choices_.recent_count) continue;
      if (choices_.recent_when) {
        trailing = FormatPlayedAgo(game.last_played_at);
        short_trailing = FormatPlayedAgoShort(game.last_played_at);
      }
    }
    games.push_back({game.id, QString::fromStdString(game.name), trailing, short_trailing});
  }
  return games;
}

SettingRow* SidebarStyleChoices::MakeStyleRow(const QString& label, bool recent) {
  auto* row = new SettingRow(label, {});
  auto* box = new QWidget(row);
  auto* tiles = new QHBoxLayout(box);
  tiles->setContentsMargins(0, 0, 0, 4);
  tiles->setSpacing(10);
  auto* group = new QButtonGroup(box);
  std::vector<QAbstractButton*>& list = recent ? recent_tiles_ : pinned_tiles_;
  for (const sidebar::StyleOption& option : sidebar::StyleOptions()) {
    const sidebar::Style style = option.style;
    auto* choice = new StyleChoice(option, [this, recent, style] { return PreviewGames(recent, style); }, artwork_, box);
    connect(choice, &QAbstractButton::clicked, this, [this, recent, style] {
      (recent ? choices_.recent : choices_.pinned) = style;
      Edited();
    });
    group->addButton(choice);
    tiles->addWidget(choice);
    list.push_back(choice);
  }
  tiles->addStretch(1);
  row->SetBelow(box);
  return row;
}

SidebarStyleCard::SidebarStyleCard(const Choices& choices, std::vector<GameSummary> pinned,
                                   std::vector<GameSummary> recent, ArtworkStore* artwork, QWidget* parent)
    : SettingsCard("Pinned and recently played", parent) {
  SetProminentTitle();
  auto* close = new QToolButton(this);
  close->setAutoRaise(true);
  icons::Follow(close, icons::Glyph::Close);
  close->setToolTip("Close");
  connect(close, &QToolButton::clicked, this, &SidebarStyleCard::CloseRequested);
  Header()->addWidget(close);

  auto* content = new SidebarStyleChoices(choices, std::move(pinned), std::move(recent), artwork, this);
  connect(content, &SidebarStyleChoices::Changed, this, &SidebarStyleCard::Changed);
  for (SettingRow* row : content->Rows()) AddRow(row);
}

}  // namespace mira_gui
