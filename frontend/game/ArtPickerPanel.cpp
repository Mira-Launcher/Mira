#include "ArtPickerPanel.h"

#include <QButtonGroup>
#include <QCache>
#include <QBuffer>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <set>

#include "../client/Events.h"
#include "../client/api/Artwork.h"
#include "../app/ErrorHelp.h"
#include "../client/EventHub.h"
#include "../theme/Theme.h"
#include "../widgets/TabRow.h"

namespace mira_gui {
namespace {

constexpr int kIdRole = Qt::UserRole;
constexpr int kStyleRole = Qt::UserRole + 1;
constexpr int kStateRole = Qt::UserRole + 2;
constexpr int kCurrentRole = Qt::UserRole + 3;
constexpr int kNsfwRole = Qt::UserRole + 4;

enum ThumbState { kIdle, kLoading, kReady, kFailed };

// Gap around each preview inside its grid cell.
constexpr int kInset = 6;
// mirad takes at most this many ids per batch.
constexpr std::size_t kMaxBatch = 64;
// The grid's item for the user's own image; candidate ids are never negative.
constexpr std::int64_t kUploadId = -1;

// "covers", "hero art", ... for messages about a slot.
QString SlotNoun(const std::string& slot) {
  return slot == "hero" ? "hero art" : slot == "logo" ? "logos" : slot == "icon" ? "icons" : "covers";
}

// A local image file, if it's one Qt can read.
bool IsImageUrl(const QUrl& url) {
  return url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty();
}

// What a candidate's chip calls it: SteamGridDB's style, or where it's from
// for a store's own art, which has none.
QString StyleLabel(const ArtCandidate& candidate) {
  if (candidate.source == "steam_cdn" || candidate.style == "steam") return "Steam";
  if (candidate.source == "epic") return "Epic";
  if (candidate.source == "lutris") return "Lutris";
  if (candidate.style.empty()) return "Other";
  QString label = QString::fromStdString(candidate.style).replace('_', ' ');
  label[0] = label[0].toUpper();
  return label;
}

class ThumbGrid : public QListWidget {
public:
  using QListWidget::QListWidget;
  std::function<void()> on_resize;

protected:
  void resizeEvent(QResizeEvent* event) override {
    QListWidget::resizeEvent(event);
    if (on_resize) on_resize();
  }
  // A click between previews drops the pick back to the art in use.
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && itemAt(event->pos()) == nullptr) setCurrentItem(nullptr);
    QListWidget::mousePressEvent(event);
  }
};

}  // namespace

class ThumbDelegate : public QStyledItemDelegate {
public:
  ThumbDelegate(QVariantAnimation* pulse, QObject* parent) : QStyledItemDelegate(parent), pulse_(pulse) {}

  // Logos and icons are shown whole on a backing; covers and heroes fill their cell.
  bool fit = false;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
    const auto* view = qobject_cast<const QListView*>(option.widget);
    return view != nullptr ? view->gridSize() : QSize(120, 180);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const theme::Tokens& tokens = theme::Current();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(option.rect).adjusted(kInset, kInset, -kInset, -kInset);
    QPainterPath path;
    path.addRoundedRect(box, tokens.radius_tile, tokens.radius_tile);

    const int state = index.data(kStateRole).toInt();
    const QPixmap pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
    if (state == kReady && !pixmap.isNull()) {
      const qreal dpr = painter->device()->devicePixelRatioF();
      const QSize target = (box.size() * dpr).toSize();
      QPixmap* scaled = scaled_.object(pixmap.cacheKey());
      if (scaled == nullptr || scaled->size() != target) {
        if (fit) {
          scaled = new QPixmap(target);
          scaled->fill(Qt::transparent);
          const QPixmap whole = pixmap.scaled(target * 0.86, Qt::KeepAspectRatio, Qt::SmoothTransformation);
          QPainter inner(scaled);
          inner.drawPixmap((target.width() - whole.width()) / 2, (target.height() - whole.height()) / 2, whole);
        } else {
          // Fills the box, cropping the overflow, like a library tile.
          const QPixmap filled = pixmap.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
          scaled = new QPixmap(filled.copy((filled.width() - target.width()) / 2,
                                           (filled.height() - target.height()) / 2, target.width(),
                                           target.height()));
        }
        scaled_.insert(pixmap.cacheKey(), scaled,
                       std::max(1, target.width() * target.height() * 4 / 1024));
      }
      if (fit) painter->fillPath(path, tokens.surface_alt);
      painter->setClipPath(path);
      painter->drawPixmap(box, *scaled, QRectF(scaled->rect()));
      painter->setClipping(false);
    } else {
      QColor fill = tokens.surface_alt;
      if (state == kLoading && pulse_ != nullptr) fill.setAlphaF(0.45 + 0.4 * pulse_->currentValue().toReal());
      painter->fillPath(path, fill);
      if (state == kFailed) {
        painter->setPen(tokens.text_muted);
        painter->drawText(box, Qt::AlignCenter | Qt::TextWordWrap, "No preview");
      }
    }

    if (option.state & QStyle::State_Selected) {
      painter->setPen(QPen(tokens.accent, 3));
      painter->drawRoundedRect(box.adjusted(-1.5, -1.5, 1.5, 1.5), tokens.radius_tile + 1.5,
                               tokens.radius_tile + 1.5);
    } else {
      QColor edge = option.state & QStyle::State_MouseOver ? tokens.text_muted : tokens.border;
      painter->setPen(QPen(edge, 1));
      painter->drawPath(path);
    }

    QFont font = painter->font();
    font.setPointSizeF(font.pointSizeF() * 0.8);
    font.setBold(true);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    // A small label in a corner of the preview.
    auto badge = [&](const QString& text, bool right, const QColor& back, const QColor& fore) {
      const qreal width = metrics.horizontalAdvance(text) + 12;
      const QRectF rect(right ? box.right() - 5 - width : box.left() + 5, box.top() + 5, width, metrics.height() + 4);
      painter->setPen(Qt::NoPen);
      painter->setBrush(back);
      painter->drawRoundedRect(rect, 4, 4);
      painter->setPen(fore);
      painter->drawText(rect, Qt::AlignCenter, text);
    };
    if (index.data(kCurrentRole).toBool()) {
      QColor back = tokens.window;
      back.setAlphaF(0.85);
      badge("Current", false, back, tokens.text);
    }
    if (index.data(kNsfwRole).toBool()) badge("NSFW", true, tokens.warning, tokens.window);
    painter->restore();
  }

private:
  QVariantAnimation* pulse_;
  // Each preview scaled to the current cell, keyed by its source's cacheKey; costs in KiB.
  // Bounded, since every candidate list browsed brings new sources.
  mutable QCache<qint64, QPixmap> scaled_{32 * 1024};
};

ArtPickerPanel::ArtPickerPanel(std::string game_id, QWidget* parent) : QWidget(parent), id_(std::move(game_id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* bar = new QWidget(this);
  bar->setObjectName("picker_bar");
  bar->setAttribute(Qt::WA_StyledBackground);
  auto* bar_layout = new QHBoxLayout(bar);
  bar_layout->setContentsMargins(16, 10, 16, 10);
  bar_layout->setSpacing(8);
  slots_ = new TabRow(bar);
  slots_->setObjectName("picker_tabs");
  slots_->AddTab("cover", "Covers");
  slots_->AddTab("hero", "Hero art");
  slots_->AddTab("logo", "Logos");
  slots_->AddTab("icon", "Icons");
  connect(slots_, &TabRow::CurrentChanged, this, [this](const QString& slot) {
    if (HasChange()) emit Previewed(QString::fromStdString(slot_), QPixmap());
    Open(slot.toStdString());
    emit PickChanged(false);
    emit SlotChanged(slot);
  });
  bar_layout->addWidget(slots_);
  bar_layout->addSpacing(6);
  chips_layout_ = new QHBoxLayout();
  chips_layout_->setSpacing(6);
  bar_layout->addLayout(chips_layout_);
  chips_ = new QButtonGroup(this);
  chips_->setExclusive(true);
  bar_layout->addStretch(1);

  // When the art is for the wrong game: SteamGridDB matched the name to
  // something else. Picking another game refetches every candidate.
  match_row_ = new QWidget(bar);
  auto* match_layout = new QHBoxLayout(match_row_);
  match_layout->setContentsMargins(0, 0, 0, 0);
  match_layout->setSpacing(6);
  auto* match_label = new QLabel("Art from", match_row_);
  match_label->setProperty("role", "muted");
  match_layout->addWidget(match_label);
  match_ = new QComboBox(match_row_);
  match_->setMinimumWidth(180);
  connect(match_, &QComboBox::activated, this, &ArtPickerPanel::ChooseMatch);
  match_layout->addWidget(match_);
  match_search_ = new QLineEdit(match_row_);
  match_search_->setPlaceholderText("Game name, then Enter");
  match_search_->setClearButtonEnabled(true);
  match_search_->setMinimumWidth(180);
  match_search_->hide();
  connect(match_search_, &QLineEdit::returnPressed, this, [this] {
    const QString query = match_search_->text().trimmed();
    if (!query.isEmpty()) LoadMatches(query);
  });
  // Leaving it empty goes back to the list.
  connect(match_search_, &QLineEdit::editingFinished, this, [this] {
    if (!match_search_->text().trimmed().isEmpty()) return;
    match_search_->hide();
    match_->show();
  });
  match_layout->addWidget(match_search_);
  match_row_->hide();
  bar_layout->addWidget(match_row_);
  auto* choose = new QPushButton("Choose image…", bar);
  choose->setToolTip("Use a PNG, JPEG or WebP of your own. You can also drop one here.");
  connect(choose, &QPushButton::clicked, this, &ArtPickerPanel::ChooseFile);
  bar_layout->addWidget(choose);
  setAcceptDrops(true);
  layout->addWidget(bar);

  stack_ = new QStackedWidget(this);
  layout->addWidget(stack_, /*stretch=*/1);

  pulse_ = new QVariantAnimation(this);
  pulse_->setStartValue(0.0);
  pulse_->setKeyValueAt(0.5, 1.0);
  pulse_->setEndValue(0.0);
  pulse_->setDuration(1400);
  pulse_->setLoopCount(-1);

  auto* grid = new ThumbGrid(stack_);
  grid_ = grid;
  grid_->setViewMode(QListView::IconMode);
  grid_->setMovement(QListView::Static);
  grid_->setResizeMode(QListView::Adjust);
  grid_->setWrapping(true);
  grid_->setUniformItemSizes(true);
  grid_->setSpacing(0);
  grid_->setSelectionMode(QAbstractItemView::SingleSelection);
  grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  grid_->verticalScrollBar()->setSingleStep(24);
  grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  grid_->setFrameShape(QFrame::NoFrame);
  grid_->setMouseTracking(true);
  grid_->setStyleSheet("QListWidget { background: transparent; padding: 8px 10px; }");
  grid_->viewport()->setAutoFillBackground(false);
  delegate_ = new ThumbDelegate(pulse_, grid_);
  grid_->setItemDelegate(delegate_);
  grid->on_resize = [this] {
    UpdateGridSize();
    QTimer::singleShot(0, this, &ArtPickerPanel::RequestVisible);
  };
  connect(pulse_, &QVariantAnimation::valueChanged, grid_->viewport(), [this] { grid_->viewport()->update(); });
  connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, &ArtPickerPanel::RequestVisible);
  connect(grid_, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem* current, QListWidgetItem*) { Pick(current); });
  connect(grid_, &QListWidget::itemActivated, this, [this] {
    if (HasChange()) emit PickActivated();
  });
  stack_->addWidget(grid_);

  message_page_ = new QWidget(stack_);
  auto* message_layout = new QVBoxLayout(message_page_);
  message_layout->addStretch(1);
  message_ = new QLabel(message_page_);
  message_->setAlignment(Qt::AlignCenter);
  message_->setWordWrap(true);
  message_->setProperty("role", "muted");
  message_layout->addWidget(message_);
  fetch_button_ = new QPushButton("Look again on SteamGridDB", message_page_);
  connect(fetch_button_, &QPushButton::clicked, this, [this] {
    fetching_ = true;
    ShowMessage("Looking on SteamGridDB…", false);
    api::RefreshMetadataAsync(this, id_, /*announce=*/false, [this](MetadataRefreshResult result) {
      if (result.ok) return;  // game.metadata_ready reloads
      fetching_ = false;
      ShowMessage("Could not look on SteamGridDB: " + error_help::Describe(result.error), true);
    });
  });
  message_layout->addWidget(fetch_button_, 0, Qt::AlignHCenter);
  message_layout->addStretch(1);
  stack_->addWidget(message_page_);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
}

void ArtPickerPanel::Open(const std::string& slot) {
  slot_ = slot;
  pick_.reset();
  candidates_.clear();
  slots_->SetCurrent(QString::fromStdString(slot_));
  ShowMessage("Loading…", false);
  if (!matches_loaded_) {
    matches_loaded_ = true;
    LoadMatches(QString());
  }
  api::GetMetadataAsync(this, id_, [this, slot](GameMetadataResult result) {
    if (slot != slot_) return;
    Populate(result);
  });
}

void ArtPickerPanel::Populate(const GameMetadataResult& result) {
  candidates_.clear();
  active_id_.reset();
  if (result.ok) {
    if (const auto found = result.metadata.candidates.find(slot_); found != result.metadata.candidates.end()) {
      candidates_ = found->second;
    }
    if (const auto found = result.metadata.active_candidate_ids.find(slot_);
        found != result.metadata.active_candidate_ids.end()) {
      active_id_ = found->second;
    }
  }
  upload_.clear();
  thumbs_.erase({slot_, kUploadId});
  delegate_->fit = slot_ == "logo" || slot_ == "icon";
  filter_.clear();
  next_page_ = 0;
  more_pages_ = true;
  page_loading_ = false;
  griddb_total_ = -1;

  grid_->blockSignals(true);
  grid_->clear();
  QListWidgetItem* current = nullptr;
  for (const ArtCandidate& candidate : candidates_) {
    QListWidgetItem* item = AddItem(candidate);
    if (item->data(kCurrentRole).toBool()) current = item;
  }
  if (current != nullptr) grid_->setCurrentItem(current);
  grid_->blockSignals(false);
  pick_ = active_id_;

  UpdateTitle();
  RebuildChips();
  UpdateGridSize();
  if (candidates_.empty()) {
    ShowMessage("Looking on SteamGridDB…", false);
  } else {
    stack_->setCurrentWidget(grid_);
    if (current != nullptr) grid_->scrollToItem(current);
    QTimer::singleShot(0, this, &ArtPickerPanel::RequestVisible);
  }
  // What the fetch cached is shown first; SteamGridDB's own pages replace it.
  RequestPage();
  emit PickChanged(false);
  emit Previewed(QString::fromStdString(slot_), QPixmap());
}

QListWidgetItem* ArtPickerPanel::AddItem(const ArtCandidate& candidate) {
  auto* item = new QListWidgetItem(grid_);
  item->setData(kIdRole, QVariant::fromValue<qlonglong>(candidate.id));
  item->setData(kStyleRole, StyleLabel(candidate));
  item->setData(kCurrentRole, active_id_ && *active_id_ == candidate.id);
  item->setData(kNsfwRole, candidate.nsfw);
  if (const auto cached = thumbs_.find({slot_, candidate.id}); cached != thumbs_.end()) {
    item->setData(Qt::DecorationRole, cached->second);
    item->setData(kStateRole, kReady);
  } else {
    item->setData(kStateRole, kIdle);
  }
  QString tip = StyleLabel(candidate);
  if (candidate.width > 0) tip += QString(" · %1×%2").arg(candidate.width).arg(candidate.height);
  if (candidate.nsfw) tip += " · marked adult on SteamGridDB";
  item->setToolTip(tip);
  item->setHidden(!filter_.isEmpty() && StyleLabel(candidate) != filter_);
  return item;
}

void ArtPickerPanel::UpdateTitle() {
  // The store's own art plus however many SteamGridDB has, once known.
  int count = static_cast<int>(candidates_.size());
  if (griddb_total_ >= 0) {
    count = griddb_total_ + static_cast<int>(std::ranges::count_if(
                                candidates_, [](const ArtCandidate& c) { return c.source != "steamgriddb"; }));
  }
  slots_->SetCount(QString::fromStdString(slot_), count);
}

void ArtPickerPanel::RequestPage() {
  if (!more_pages_ || page_loading_) return;
  page_loading_ = true;
  const std::string slot = slot_;
  page_request_ = std::to_string(QRandomGenerator::global()->generate64());
  api::FetchArtCandidatesAsync(this, id_, slot, next_page_, page_request_, [this, slot](GameActionResult result) {
    if (result.ok || slot != slot_) return;  // game.artwork_candidates_ready follows
    page_loading_ = false;
    more_pages_ = false;
    if (candidates_.empty()) ShowMessage(QString("No other %1 found for this game.").arg(SlotNoun(slot)), true);
  });
}

void ArtPickerPanel::ShowPage(const ArtCandidatesEvent& event) {
  page_loading_ = false;
  if (!event.error.empty()) {
    // No key or no match: what the fetch cached is all there is.
    more_pages_ = false;
    if (candidates_.empty()) {
      ShowMessage(QString("No other %1 found for this game.").arg(SlotNoun(slot_)), true);
    }
    return;
  }
  std::set<std::int64_t> live;
  for (const ArtCandidate& candidate : event.candidates) live.insert(candidate.id);
  grid_->blockSignals(true);
  if (event.page == 0) {
    // SteamGridDB's answer, in its order, replaces what was cached, which may
    // predate steamgriddb.nsfw changing. The one in use stays either way.
    for (int i = grid_->count() - 1; i >= 0; --i) {
      const std::int64_t id = grid_->item(i)->data(kIdRole).toLongLong();
      const auto candidate = std::ranges::find(candidates_, id, &ArtCandidate::id);
      if (candidate == candidates_.end() || candidate->source != "steamgriddb") continue;
      if (active_id_ == id && !live.contains(id)) continue;
      if (pick_ == id && !live.contains(id)) pick_ = active_id_;
      candidates_.erase(candidate);
      delete grid_->takeItem(i);
    }
  }
  for (const ArtCandidate& candidate : event.candidates) {
    if (std::ranges::find(candidates_, candidate.id, &ArtCandidate::id) != candidates_.end()) continue;
    candidates_.push_back(candidate);
    QListWidgetItem* item = AddItem(candidate);
    if (pick_ == candidate.id) grid_->setCurrentItem(item);
  }
  grid_->blockSignals(false);
  griddb_total_ = event.total;
  next_page_ = event.page + 1;
  const auto loaded = std::ranges::count_if(candidates_, [](const ArtCandidate& c) { return c.source == "steamgriddb"; });
  more_pages_ = !event.candidates.empty() && loaded < event.total;
  UpdateTitle();
  RebuildChips();
  if (candidates_.empty()) {
    ShowMessage(QString("No other %1 found for this game.").arg(SlotNoun(slot_)), true);
    return;
  }
  stack_->setCurrentWidget(grid_);
  QTimer::singleShot(0, this, &ArtPickerPanel::RequestVisible);
}

void ArtPickerPanel::RebuildChips() {
  for (QAbstractButton* chip : chips_->buttons()) {
    chips_->removeButton(chip);
    chip->deleteLater();
  }
  std::vector<std::pair<QString, int>> styles;
  for (const ArtCandidate& candidate : candidates_) {
    const QString label = StyleLabel(candidate);
    auto found = std::ranges::find(styles, label, &std::pair<QString, int>::first);
    if (found == styles.end()) {
      styles.emplace_back(label, 1);
    } else {
      ++found->second;
    }
  }
  // One style is nothing to filter by.
  if (styles.size() < 2) return;
  styles.insert(styles.begin(), {QString(), static_cast<int>(candidates_.size())});
  for (const auto& [label, count] : styles) {
    auto* chip = new QPushButton(QString("%1  %2").arg(label.isEmpty() ? "All" : label).arg(count), this);
    chip->setObjectName("chip");
    chip->setCheckable(true);
    chip->setChecked(label == filter_);
    chip->setCursor(Qt::PointingHandCursor);
    connect(chip, &QPushButton::clicked, this, [this, label] { ApplyFilter(label); });
    chips_->addButton(chip);
    chips_layout_->addWidget(chip);
  }
}

void ArtPickerPanel::ApplyFilter(const QString& style) {
  filter_ = style;
  for (int i = 0; i < grid_->count(); ++i) {
    QListWidgetItem* item = grid_->item(i);
    // The user's own image has no style, and stays in sight while it's there.
    const bool upload = item->data(kIdRole).toLongLong() == kUploadId;
    item->setHidden(!upload && !style.isEmpty() && item->data(kStyleRole).toString() != style);
  }
  grid_->verticalScrollBar()->setValue(0);
  // The grid lays the rest out again only once control returns to it.
  QTimer::singleShot(0, this, &ArtPickerPanel::RequestVisible);
}

void ArtPickerPanel::RequestVisible() {
  if (candidates_.empty() || stack_->currentWidget() != grid_) return;
  // A screen ahead, so scrolling finds previews already there.
  const QRect ahead = grid_->viewport()->rect().adjusted(0, 0, 0, grid_->viewport()->height());
  std::vector<std::int64_t> ids;
  QListWidgetItem* last_shown = nullptr;
  for (int i = 0; i < grid_->count(); ++i) {
    QListWidgetItem* item = grid_->item(i);
    if (item->isHidden()) continue;
    last_shown = item;
    if (ids.size() >= kMaxBatch || item->data(kStateRole).toInt() != kIdle) continue;
    if (!grid_->visualItemRect(item).intersects(ahead)) continue;
    item->setData(kStateRole, kLoading);
    ids.push_back(item->data(kIdRole).toLongLong());
  }
  // The end of what's loaded is within a screen: SteamGridDB's next page.
  if (last_shown == nullptr || grid_->visualItemRect(last_shown).intersects(ahead)) RequestPage();
  if (ids.empty()) return;
  UpdatePulse();
  const std::string slot = slot_;
  api::FetchArtThumbsAsync(this, id_, slot, ids, [this, slot, ids](GameActionResult result) {
    if (!result.ok && slot == slot_) MarkFailed(ids);
  });
}

void ArtPickerPanel::ShowThumbs(const std::string& slot, const std::vector<std::pair<std::int64_t, QImage>>& images) {
  for (const auto& [candidate_id, image] : images) {
    const QPixmap pixmap = QPixmap::fromImage(image);
    thumbs_[{slot, candidate_id}] = pixmap;
    if (slot != slot_) continue;
    if (QListWidgetItem* item = ItemFor(candidate_id)) {
      item->setData(Qt::DecorationRole, pixmap);
      item->setData(kStateRole, kReady);
    }
    if (pick_ == candidate_id) EmitPreview();
  }
}

void ArtPickerPanel::MarkFailed(const std::vector<std::int64_t>& ids) {
  for (const std::int64_t candidate_id : ids) {
    if (QListWidgetItem* item = ItemFor(candidate_id); item != nullptr && item->data(kStateRole).toInt() != kReady) {
      item->setData(kStateRole, kFailed);
    }
  }
  UpdatePulse();
}

QListWidgetItem* ArtPickerPanel::ItemFor(std::int64_t id) const {
  for (int i = 0; i < grid_->count(); ++i) {
    if (grid_->item(i)->data(kIdRole).toLongLong() == id) return grid_->item(i);
  }
  return nullptr;
}

void ArtPickerPanel::Pick(QListWidgetItem* item) {
  if (item == nullptr) {
    pick_ = active_id_;
  } else {
    pick_ = item->data(kIdRole).toLongLong();
  }
  emit PickChanged(HasChange());
  EmitPreview();
}

void ArtPickerPanel::EmitPreview() {
  const QString slot = QString::fromStdString(slot_);
  if (!HasChange()) {
    emit Previewed(slot, QPixmap());
    return;
  }
  // Not loaded yet: ShowThumbs calls back here when it is.
  if (const auto found = thumbs_.find({slot_, *pick_}); found != thumbs_.end()) emit Previewed(slot, found->second);
}

bool ArtPickerPanel::HasChange() const { return pick_.has_value() && pick_ != active_id_; }

void ArtPickerPanel::ResetPick() {
  QListWidgetItem* active = active_id_ ? ItemFor(*active_id_) : nullptr;
  if (active != nullptr) {
    grid_->setCurrentItem(active);  // picks it, through currentItemChanged
    grid_->scrollToItem(active);
    return;
  }
  grid_->blockSignals(true);
  grid_->setCurrentItem(nullptr);
  grid_->blockSignals(false);
  Pick(nullptr);
}

void ArtPickerPanel::Apply() {
  if (!HasChange()) return;
  const std::string slot = slot_;
  const std::int64_t candidate_id = *pick_;
  applying_ = {slot, candidate_id};
  auto done = [this, slot](ArtworkSelectResult result) {
    if (result.ok) return;  // game.artwork_selected follows
    applying_.reset();
    emit ApplyFailed(QString::fromStdString(slot), error_help::Describe(result.error));
  };
  if (candidate_id == kUploadId) {
    api::UploadArtworkAsync(this, id_, slot, upload_, std::move(done));
  } else {
    api::SelectArtworkAsync(this, id_, slot, candidate_id, std::move(done));
  }
}

void ArtPickerPanel::ChooseFile() {
  const QString path = QFileDialog::getOpenFileName(this, "Choose an image", QString(),
                                                    "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)");
  if (!path.isEmpty()) UseFile(path);
}

void ArtPickerPanel::dragEnterEvent(QDragEnterEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.size() == 1 && IsImageUrl(urls.front())) event->acceptProposedAction();
}

void ArtPickerPanel::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.size() != 1 || !IsImageUrl(urls.front())) return;
  event->acceptProposedAction();
  UseFile(urls.front().toLocalFile());
}

void ArtPickerPanel::UseFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    emit ApplyFailed(QString::fromStdString(slot_), "Could not open " + path + ": " + file.errorString());
    return;
  }
  const QByteArray bytes = file.readAll();
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull()) {
    emit ApplyFailed(QString::fromStdString(slot_), "Could not read " + path + ": " + reader.errorString());
    return;
  }
  // mirad reads PNG and JPEG, and ignores EXIF rotation; anything else goes as PNG, upright.
  if ((bytes.startsWith("\x89PNG") || bytes.startsWith("\xFF\xD8\xFF")) &&
      reader.transformation() == QImageIOHandler::TransformationNone) {
    upload_ = bytes.toStdString();
  } else {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    upload_ = png.toStdString();
  }
  const QPixmap pixmap = QPixmap::fromImage(image.width() > 1920 || image.height() > 1920
                                                ? image.scaled(1920, 1920, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                                : image);
  thumbs_[{slot_, kUploadId}] = pixmap;

  QListWidgetItem* item = ItemFor(kUploadId);
  if (item == nullptr) {
    item = new QListWidgetItem();
    item->setData(kIdRole, QVariant::fromValue<qlonglong>(kUploadId));
    item->setData(kStyleRole, QString("Yours"));
    grid_->insertItem(0, item);
  }
  item->setData(Qt::DecorationRole, pixmap);
  item->setData(kStateRole, kReady);
  item->setData(kCurrentRole, false);
  item->setToolTip(QFileInfo(path).fileName() + QString(" · %1×%2").arg(image.width()).arg(image.height()));
  item->setHidden(false);
  stack_->setCurrentWidget(grid_);
  grid_->setCurrentItem(item);  // picks it, through currentItemChanged
  grid_->scrollToItem(item);
  if (pick_ == kUploadId) EmitPreview();  // the same item again, with a new image
}

void ArtPickerPanel::UpdateGridSize() {
  // Sized as if the scroll bar were always there, so it showing up doesn't
  // change the column count and lay everything out again.
  const int scroll_bar = grid_->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  const int width = grid_->width() - 20 - scroll_bar;  // less the grid's padding
  // SteamGridDB heroes are 96:31, covers 2:3; logos vary, so a wide box, and icons are square.
  const bool hero = slot_ == "hero";
  const bool logo = slot_ == "logo";
  const bool icon = slot_ == "icon";
  const int columns = std::max(2, width / (hero ? 230 : logo ? 200 : icon ? 96 : 118));
  const int cell = std::max(40, width / columns);
  const int art = cell - 2 * kInset;
  const int height = qRound(art * (hero ? 31.0 / 96.0 : logo ? 0.5 : icon ? 1.0 : 1.5)) + 2 * kInset;
  grid_->setGridSize(QSize(cell, height));
}

void ArtPickerPanel::UpdatePulse() {
  bool loading = false;
  for (int i = 0; i < grid_->count() && !loading; ++i) loading = grid_->item(i)->data(kStateRole).toInt() == kLoading;
  if (loading && pulse_->state() != QAbstractAnimation::Running) pulse_->start();
  if (!loading) pulse_->stop();
}

void ArtPickerPanel::ShowMessage(const QString& text, bool offer_fetch) {
  message_->setText(text);
  fetch_button_->setVisible(offer_fetch);
  stack_->setCurrentWidget(message_page_);
}

void ArtPickerPanel::LoadMatches(const QString& query) {
  match_->setEnabled(false);
  api::GetGriddbMatchesAsync(this, id_, query.toStdString(), [this, query](GriddbMatchesResult result) {
    match_->setEnabled(true);
    match_search_->hide();
    match_->show();
    // No key, or SteamGridDB unreachable: nothing to choose between.
    if (!result.ok) {
      if (!query.isEmpty()) ShowMessage("Could not search SteamGridDB: " + error_help::Describe(result.error), false);
      return;
    }
    match_row_->show();
    match_->clear();
    for (const GriddbMatch& match : result.matches) {
      QString label = QString::fromStdString(match.name);
      if (match.year > 0) label += QString(" (%1)").arg(match.year);
      match_->addItem(label, QVariant::fromValue<qlonglong>(match.id));
      if (match.id == result.chosen) match_->setCurrentIndex(match_->count() - 1);
    }
    if (result.matches.empty()) {
      match_->addItem("No matches for \"" + QString::fromStdString(result.query) + "\"");
    }
    match_->insertSeparator(match_->count());
    match_->addItem("Search another name…");
    // A search's results aren't in use until one is picked.
    if (!query.isEmpty() && !result.matches.empty()) {
      match_->setCurrentIndex(-1);
      match_->setPlaceholderText("Pick the right game…");
      match_->showPopup();
    }
  });
}

void ArtPickerPanel::ChooseMatch(int index) {
  if (index == match_->count() - 1) {
    match_->hide();
    match_search_->show();
    match_search_->setFocus();
    return;
  }
  const QVariant id = match_->itemData(index);
  if (!id.isValid() || fetching_) return;
  fetching_ = true;
  ShowMessage("Fetching art for " + match_->itemText(index) + "…", false);
  api::SetGriddbMatchAsync(this, id_, id.toLongLong(), [this](GameActionResult result) {
    if (result.ok) return;  // game.metadata_ready reloads
    fetching_ = false;
    ShowMessage("Could not switch games: " + error_help::Describe(result.error), false);
  });
}

void ArtPickerPanel::HandleEvent(const std::string& type, const std::string& data) {
  if (type == "game.artwork_candidates_ready") {
    ArtCandidatesEvent event;
    if (!events::ParseArtCandidatesEvent(data, &event) || event.id != id_ || event.slot != slot_) return;
    if (!page_loading_ || event.request != page_request_) return;  // an earlier Open()'s, or a replay
    ShowPage(event);
    return;
  }

  if (type == "game.artwork_thumbs_ready") {
    ArtThumbsEvent event;
    if (!events::ParseArtThumbsEvent(data, &event) || event.id != id_ || event.slot != slot_) return;
    MarkFailed(event.failed);
    if (event.ready.empty()) return;
    const std::string slot = event.slot;
    const std::vector<std::int64_t> ready = event.ready;
    api::GetArtThumbsAsync(this, id_, slot, ready, [this, slot, ready](std::vector<std::pair<std::int64_t, QImage>> images) {
      ShowThumbs(slot, images);
      if (slot != slot_) return;
      std::vector<std::int64_t> missing;
      for (const std::int64_t candidate_id : ready) {
        if (!thumbs_.contains({slot, candidate_id})) missing.push_back(candidate_id);
      }
      MarkFailed(missing);
    });
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    MetadataEvent event;
    if (!fetching_ || !events::ParseMetadataEvent(data, &event) || event.id != id_) return;
    fetching_ = false;
    if (type == "game.metadata_failed") {
      ShowMessage("Could not fetch from SteamGridDB: " + error_help::Describe(event.error), true);
      return;
    }
    Open(slot_);
    return;
  }

  if (type != "game.artwork_selected" && type != "game.artwork_select_failed") return;
  ArtworkSelectEvent event;
  if (!events::ParseArtworkSelectEvent(data, &event) || event.id != id_) return;
  if (!applying_ || applying_->first != event.slot) return;
  const std::int64_t applied = applying_->second;
  applying_.reset();
  if (type == "game.artwork_select_failed") {
    emit ApplyFailed(QString::fromStdString(event.slot), error_help::Describe(event.error));
    return;
  }
  if (event.slot != slot_) return;
  active_id_ = applied;
  for (int i = 0; i < grid_->count(); ++i) {
    grid_->item(i)->setData(kCurrentRole, grid_->item(i)->data(kIdRole).toLongLong() == applied);
  }
  emit PickChanged(HasChange());
}

}  // namespace mira_gui
