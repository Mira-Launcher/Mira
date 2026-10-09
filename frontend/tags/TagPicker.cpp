#include "TagPicker.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Tags.h"
#include "../library/ArtworkStore.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/FlowLayout.h"
#include "../widgets/Labels.h"
#include "../widgets/Scrolling.h"

namespace mira_gui {

// One game's cover, ticked or not; a click toggles it.
class PickTile : public QAbstractButton {
public:
  static constexpr int kName = 20;

  PickTile(const GameSummary& game, ArtworkStore* artwork, int width, QWidget* parent)
      : QAbstractButton(parent), game_(game), artwork_(artwork), width_(width) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QString::fromStdString(game.name));
    setAccessibleName(QString::fromStdString(game.name));
  }

  const GameSummary& Game() const { return game_; }
  void SetWidth(int width) {
    if (width == width_) return;
    width_ = width;
    updateGeometry();
    update();
  }
  // The library grid's tile shape, so the covers it already scaled are reused.
  QSize CoverSize() const { return {width_, width_ * 3 / 2}; }
  QSize sizeHint() const override { return {width_, CoverSize().height() + 4 + kName}; }

protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF cover(QPointF(0, 0), CoverSize());
    const qreal radius = tokens.radius_tile;
    painter.save();
    if (!isChecked()) painter.setOpacity(0.55);
    QPainterPath clip;
    clip.addRoundedRect(cover, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(cover.toRect(), artwork_->Cover(game_, CoverSize(), devicePixelRatioF()));
    painter.restore();

    const QRectF box(width_ - 26, 6, 20, 20);
    if (isChecked()) {
      painter.setPen(QPen(tokens.accent, 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(cover.adjusted(1, 1, -1, -1), radius, radius);
      painter.setPen(Qt::NoPen);
      painter.setBrush(tokens.accent);
      painter.drawRoundedRect(box, 5, 5);
      painter.setPen(QPen(tokens.on_accent, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPolyline(QPolygonF({box.topLeft() + QPointF(5, 10), box.topLeft() + QPointF(8.5, 13.5),
                                      box.topLeft() + QPointF(15, 6.5)}));
    } else {
      QColor shade = tokens.window;
      shade.setAlphaF(0.6);
      painter.setBrush(shade);
      painter.setPen(QPen(tokens.text, 1));
      painter.drawRoundedRect(box.adjusted(1, 1, -1, -1), 5, 5);
    }
    if (hasFocus()) {
      painter.setPen(QPen(tokens.text, 1, Qt::DotLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(cover.adjusted(3, 3, -3, -3), radius, radius);
    }

    painter.setPen(isChecked() ? tokens.text : tokens.text_muted);
    const QRect name(0, CoverSize().height() + 4, width_, kName);
    painter.drawText(
        name, Qt::AlignLeft | Qt::AlignVCenter,
        fontMetrics().elidedText(QString::fromStdString(game_.name), Qt::ElideRight, width_));
  }

private:
  GameSummary game_;
  ArtworkStore* artwork_;
  int width_;
};

TagPicker::TagPicker(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* header_box = new QWidget(this);
  header_box->setObjectName("settings_canvas");
  auto* header = new QHBoxLayout(header_box);
  header->setContentsMargins(24, 20, 32, 10);
  header->setSpacing(8);
  // As Settings' own back arrow.
  auto* back = new QToolButton(header_box);
  back->setAutoRaise(true);
  icons::Follow(back, icons::Glyph::ArrowLeft);
  back->setToolTip("Back to Tags");
  back->setAccessibleName("Back to Tags");
  connect(back, &QToolButton::clicked, this, &TagPicker::Closed);
  header->addWidget(back);
  title_ = new QLabel(header_box);
  title_->setObjectName("page_title");
  header->addWidget(title_);
  header->addSpacing(4);
  name_edit_ = new QLineEdit(header_box);
  name_edit_->setPlaceholderText("Tag name");
  name_edit_->setFixedWidth(240);
  connect(name_edit_, &QLineEdit::textChanged, this, &TagPicker::UpdateBar);
  header->addWidget(name_edit_);
  header->addStretch(1);
  search_ = new QLineEdit(header_box);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search games");
  search_->setClearButtonEnabled(true);
  search_->setFixedWidth(240);
  connect(search_, &QLineEdit::textChanged, this, &TagPicker::Filter);
  header->addWidget(search_);
  outer->addWidget(header_box);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("settings_page");
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll_, this);
  scroll_->viewport()->installEventFilter(this);
  content_ = new QWidget();
  content_->setObjectName("settings_canvas");
  sections_ = new QVBoxLayout(content_);
  // Room at the bottom so the floating bar never covers the last covers.
  sections_->setContentsMargins(32, 6, 32, 90);
  sections_->setSpacing(10);
  scroll_->setWidget(content_);
  outer->addWidget(scroll_, /*stretch=*/1);

  // Floats over the bottom, like the change bar it looks like.
  bar_ = new QFrame(this);
  bar_->setObjectName("change_bar");
  auto* bar = new QHBoxLayout(bar_);
  bar->setContentsMargins(16, 8, 8, 8);
  bar->setSpacing(10);
  folder_ = new Switch(bar_);
  connect(folder_, &Switch::toggled, this, &TagPicker::UpdateBar);
  bar->addWidget(folder_);
  folder_label_ = new QLabel(bar_);
  folder_label_->setToolTip("A folder for it in each library folder sorted by tag (Tag settings)");
  bar->addWidget(folder_label_);
  bar->addSpacing(6);
  auto* cancel = new QPushButton("Cancel", bar_);
  connect(cancel, &QPushButton::clicked, this, &TagPicker::Closed);
  bar->addWidget(cancel);
  apply_ = new QPushButton(bar_);
  apply_->setDefault(true);
  connect(apply_, &QPushButton::clicked, this, &TagPicker::Apply);
  bar->addWidget(apply_);

  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  escape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escape, &QShortcut::activated, this, &TagPicker::Closed);
  connect(artwork_, &ArtworkStore::QuickScalingEnded, this, [this] {
    for (PickTile* tile : tiles_) tile->update();
  });
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    for (PickTile* tile : tiles_) {
      if (QString::fromStdString(tile->Game().id) == id) tile->update();
    }
  });
}

bool TagPicker::eventFilter(QObject* watched, QEvent* event) {
  if (watched == scroll_->viewport() && event->type() == QEvent::Wheel) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (wheel->modifiers() & Qt::ControlModifier) {
      if (const int steps = wheel->angleDelta().y() / 120; steps != 0) emit ZoomRequested(steps);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

TileRow TagPicker::Row() const {
  for (const QWidget* grid : content_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
    if (grid->isVisible() && dynamic_cast<const FlowLayout*>(grid->layout()) != nullptr) {
      return {RoomBesideScrollbar(grid, grid->width()), kTileGap};
    }
  }
  return {};
}

void TagPicker::SetTileWidth(int width) {
  tile_width_ = width;
  for (PickTile* tile : tiles_) tile->SetWidth(width);
}

void TagPicker::ToggleHidden() {
  show_hidden_ = !show_hidden_;
  Filter();
  // Shown: brought into view, since it's the last section.
  if (show_hidden_ && hidden_section_ != nullptr && !hidden_section_->isHidden()) {
    QWidget* heading = sections_->itemAt(sections_->indexOf(hidden_section_) - 1)->widget();
    scroll_->ensureWidgetVisible(heading != nullptr ? heading : hidden_section_, 0, 0);
  }
}

void TagPicker::Open(const QString& name, bool existing, std::vector<Section> sections,
                     const std::vector<GameSummary>& games, bool folder, std::vector<TagSummary> known) {
  name_ = name;
  existing_ = existing;
  folder_was_ = folder;
  show_hidden_ = false;
  known_ = std::move(known);
  had_.clear();
  kept_.clear();
  std::vector<std::string> ticked;
  for (const Section& section : sections) {
    if (!section.ticked) continue;
    ticked.insert(ticked.end(), section.ids.begin(), section.ids.end());
    if (existing) had_.insert(had_.end(), section.ids.begin(), section.ids.end());
  }
  title_->setText(name.isEmpty() ? QString("New tag") : existing ? QString("Choose games for %1").arg(name)
                                                                 : QString("Add %1").arg(name));
  name_edit_->setVisible(name.isEmpty());
  name_edit_->clear();
  search_->clear();
  folder_->setChecked(folder);
  folder_label_->setText(existing ? "A folder" : "Also a folder");

  tiles_.clear();
  hidden_section_ = nullptr;
  while (QLayoutItem* item = sections_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }

  // A store's launcher isn't a game to tag; one that had the tag keeps it.
  std::vector<const GameSummary*> sorted;
  for (const GameSummary& game : games) {
    if (game.source == "launcher") {
      if (std::ranges::contains(had_, game.id)) kept_.push_back(game.id);
      continue;
    }
    sorted.push_back(&game);
  }
  std::ranges::sort(sorted, [](const GameSummary* a, const GameSummary* b) {
    return QString::fromStdString(a->name).compare(QString::fromStdString(b->name), Qt::CaseInsensitive) < 0;
  });
  std::vector<const GameSummary*> hidden;
  std::erase_if(sorted, [&](const GameSummary* game) {
    if (!IsHidden(*game)) return false;
    hidden.push_back(game);
    return true;
  });
  std::vector<std::string> placed;
  for (const Section& section : sections) {
    std::vector<const GameSummary*> in;
    for (const GameSummary* game : sorted) {
      if (std::ranges::contains(section.ids, game->id) && !std::ranges::contains(placed, game->id)) {
        in.push_back(game);
        placed.push_back(game->id);
      }
    }
    // Its hidden games wait in the Hidden section, but count here, as on the Tags page.
    const auto hidden_in = std::ranges::count_if(
        hidden, [&](const GameSummary* game) { return std::ranges::contains(section.ids, game->id); });
    if (!in.empty()) {
      const QString count = hidden_in > 0 ? QString("%1 + %2 hidden").arg(in.size()).arg(hidden_in)
                                          : QString::number(in.size());
      AddSection(QString("%1 · %2").arg(section.heading, count), in,
                 section.ticked ? section.ids : std::vector<std::string>{});
    }
  }
  std::vector<const GameSummary*> rest;
  for (const GameSummary* game : sorted) {
    if (!std::ranges::contains(placed, game->id)) rest.push_back(game);
  }
  if (!rest.empty())
    AddSection(placed.empty() ? QString("Your games") : QString("Your other games"), rest, {});
  // Ticked as any other game would be, so the count matches the Tags page's even out of sight.
  if (!hidden.empty())
    hidden_section_ = AddSection(QString("Hidden · %1").arg(hidden.size()), hidden, ticked);
  sections_->addStretch(1);
  Filter();
  UpdateBar();
  if (name.isEmpty()) {
    name_edit_->setFocus();
  } else {
    search_->setFocus();
  }
}

QWidget* TagPicker::AddSection(const QString& heading, const std::vector<const GameSummary*>& games,
                               const std::vector<std::string>& ticked) {
  auto* label = MakeGroupHeading(content_, heading.toUpper());
  sections_->addWidget(label);
  auto* grid = new QWidget(content_);
  auto* flow = new FlowLayout(grid, kTileGap);
  for (const GameSummary* game : games) {
    auto* tile = new PickTile(*game, artwork_, tile_width_, grid);
    tile->setChecked(std::ranges::contains(ticked, game->id));
    connect(tile, &PickTile::toggled, this, &TagPicker::UpdateBar);
    flow->addWidget(tile);
    tiles_.append(tile);
  }
  sections_->addWidget(grid);
  return grid;
}

void TagPicker::Filter() {
  const QString text = search_->text().trimmed();
  for (PickTile* tile : tiles_) {
    tile->setVisible(text.isEmpty() || QString::fromStdString(tile->Game().name).contains(text, Qt::CaseInsensitive));
  }
  // A section whose every cover is filtered out hides with its heading; the hidden games' section
  // shows only after Ctrl+H.
  for (int i = 0; i + 1 < sections_->count(); i += 2) {
    QWidget* heading = sections_->itemAt(i)->widget();
    QWidget* grid = sections_->itemAt(i + 1)->widget();
    if (heading == nullptr || grid == nullptr) continue;
    const bool any = std::ranges::any_of(
        tiles_, [grid](const PickTile* t) { return t->parentWidget() == grid && !t->isHidden(); });
    const bool shown = any && (grid != hidden_section_ || show_hidden_);
    heading->setVisible(shown);
    grid->setVisible(shown);
  }
}

std::vector<std::string> TagPicker::Chosen() const {
  std::vector<std::string> ids = kept_;
  for (const PickTile* tile : tiles_) {
    if (tile->isChecked()) ids.push_back(tile->Game().id);
  }
  return ids;
}

void TagPicker::UpdateBar() {
  int count = 0;
  for (const PickTile* tile : tiles_) count += tile->isChecked();
  const QString games = count == 1 ? QString("1 game") : QString("%1 games").arg(count);
  if (existing_) {
    std::vector<std::string> now = Chosen();
    std::vector<std::string> had = had_;
    std::ranges::sort(now);
    std::ranges::sort(had);
    apply_->setText(QString("Save, %1").arg(games));
    apply_->setEnabled(now != had || folder_->isChecked() != folder_was_);
  } else {
    apply_->setText(QString("Add to %1").arg(games));
    apply_->setEnabled(count > 0 && (!name_.isEmpty() || !name_edit_->text().trimmed().isEmpty()));
  }
  bar_->adjustSize();
  bar_->move((width() - bar_->width()) / 2, height() - bar_->height() - 18);
  bar_->raise();
}

void TagPicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  UpdateBar();
}

void TagPicker::Apply() {
  const std::string name = (name_.isEmpty() ? name_edit_->text().trimmed() : name_).toStdString();
  if (name.empty()) return;
  std::vector<std::string> ids = Chosen();
  // A new name that's already a tag adds to it rather than taking it off the games that have it.
  if (!existing_) {
    const auto known = std::ranges::find_if(known_, [&](const TagSummary& t) { return SameTag(t.name, name); });
    if (known != known_.end()) {
      for (const std::string& id : known->ids) {
        if (!std::ranges::contains(ids, id)) ids.push_back(id);
      }
    }
  }
  const std::optional<bool> folder =
      folder_->isChecked() != folder_was_ ? std::optional<bool>(folder_->isChecked()) : std::nullopt;
  apply_->setEnabled(false);
  api::SetTagAsync(this, name, ids, folder, [this](PatchGamesResult result) {
    if (!result.ok) {
      UpdateBar();
      notify::FailedRequest(window(), "Could not change that tag.", result.error);
      return;
    }
    emit Applied(result.games);
    emit Closed();
  });
}

}  // namespace mira_gui
