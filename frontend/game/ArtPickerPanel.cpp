#include "ArtPickerPanel.h"

#include <QBuffer>
#include <QButtonGroup>
#include <QCache>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <functional>
#include <limits>
#include <set>

#include "../app/ErrorHelp.h"
#include "../client/Async.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Artwork.h"
#include "../theme/Theme.h"
#include "../widgets/FlowLayout.h"
#include "../widgets/TabRow.h"

namespace mira_gui {
namespace {

constexpr int kIdRole = Qt::UserRole;
constexpr int kStyleRole = Qt::UserRole + 1;
constexpr int kStateRole = Qt::UserRole + 2;
constexpr int kCurrentRole = Qt::UserRole + 3;
constexpr int kNsfwRole = Qt::UserRole + 4;
constexpr int kTextRole = Qt::UserRole + 5;  // shown on an item without an image

enum ThumbState { kIdle, kLoading, kReady, kFailed };

// Gap around each preview inside its grid cell.
constexpr int kInset = 6;
// mirad takes at most this many ids per batch.
constexpr std::size_t kMaxBatch = 64;
// The grid's items that aren't candidates: the user's own image, and no art at all.
// Candidate ids can be negative (a store's own art is -1), so these sit far below.
constexpr std::int64_t kUploadId = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kNoneId = kUploadId + 1;
// A cell for a candidate SteamGridDB has and that isn't loaded yet.
constexpr std::int64_t kPlaceholderId = kUploadId + 2;
// The largest image downloaded from a URL.
constexpr qint64 kMaxDownload = 32 * 1024 * 1024;

bool IsOwnItem(std::int64_t id) {
  return id == kUploadId || id == kNoneId;
}

// "covers", "hero art", ... for messages about a slot.
QString SlotNoun(const std::string& slot) {
  return slot == "hero"   ? "hero art"
         : slot == "logo" ? "logos"
         : slot == "icon" ? "icons"
                          : "covers";
}

QString SlotSingular(const std::string& slot) {
  return slot == "hero" ? "hero art" : QString::fromStdString(slot);
}

// A local image file, if it's one Qt can read.
bool IsImageUrl(const QUrl& url) {
  return url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty();
}

bool IsWebUrl(const QUrl& url) {
  return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") &&
         !url.host().isEmpty();
}

// A SteamGridDB style as its chip says it: "white_logo" is "White logo".
QString StyleName(const std::string& style) {
  QString label = QString::fromStdString(style).replace('_', ' ');
  if (!label.isEmpty()) label[0] = label[0].toUpper();
  return label;
}

// Art that isn't from SteamGridDB's list, so its pages and counts leave it out.
bool IsLocal(const ArtCandidate& candidate) {
  return candidate.source != "steamgriddb" || candidate.style.empty();
}

// What a candidate's chip calls it: SteamGridDB's style, or where it's from
// for a store's own art, which has none.
QString StyleLabel(const ArtCandidate& candidate) {
  if (candidate.source == "steam_cdn" || candidate.style == "steam") return "Steam";
  if (candidate.source == "epic") return "Epic";
  if (candidate.source == "lutris") return "Lutris";
  if (candidate.style.empty()) return "Other";
  return StyleName(candidate.style);
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
    if (event->button() == Qt::LeftButton && itemAt(event->pos()) == nullptr)
      setCurrentItem(nullptr);
    QListWidget::mousePressEvent(event);
  }
};

}  // namespace

class ThumbDelegate : public QStyledItemDelegate {
 public:
  ThumbDelegate(QVariantAnimation* pulse, QObject* parent)
      : QStyledItemDelegate(parent), pulse_(pulse) {}

  // Logos and icons are shown whole on a backing; covers and heroes fill their cell.
  bool fit = false;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
    const auto* view = qobject_cast<const QListView*>(option.widget);
    return view != nullptr ? view->gridSize() : QSize(120, 180);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
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
      QPixmap shown;
      if (scaled == nullptr || scaled->size() != target) {
        if (fit) {
          scaled = new QPixmap(target);
          scaled->fill(Qt::transparent);
          const QPixmap whole =
              pixmap.scaled(target * 0.86, Qt::KeepAspectRatio, Qt::SmoothTransformation);
          QPainter inner(scaled);
          inner.drawPixmap((target.width() - whole.width()) / 2,
                           (target.height() - whole.height()) / 2, whole);
        } else {
          // Fills the box, cropping the overflow, like a library tile.
          const QPixmap filled =
              pixmap.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
          scaled = new QPixmap(filled.copy((filled.width() - target.width()) / 2,
                                           (filled.height() - target.height()) / 2, target.width(),
                                           target.height()));
        }
        // insert() can delete the pixmap at once when it costs more than maxCost.
        shown = *scaled;
        scaled_.insert(pixmap.cacheKey(), scaled,
                       std::max(1, target.width() * target.height() * 4 / 1024));
      }
      if (shown.isNull()) shown = *scaled;
      if (fit) painter->fillPath(path, tokens.surface_alt);
      painter->setClipPath(path);
      painter->drawPixmap(box, shown, QRectF(shown.rect()));
      painter->setClipping(false);
    } else {
      QColor fill = tokens.surface_alt;
      if (state == kLoading && pulse_ != nullptr)
        fill.setAlphaF(0.45 + 0.4 * pulse_->currentValue().toReal());
      painter->fillPath(path, fill);
      const QString text = state == kFailed ? "No preview" : index.data(kTextRole).toString();
      if (!text.isEmpty()) {
        painter->setPen(tokens.text_muted);
        painter->drawText(box, Qt::AlignCenter | Qt::TextWordWrap, text);
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
      const QRectF rect(right ? box.right() - 5 - width : box.left() + 5, box.top() + 5, width,
                        metrics.height() + 4);
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

ArtPickerPanel::ArtPickerPanel(std::string game_id, QWidget* parent, bool title)
    : QWidget(parent), id_(std::move(game_id)), title_(title) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* bar = new QWidget(this);
  bar->setObjectName("picker_bar");
  bar->setAttribute(Qt::WA_StyledBackground);
  // Two rows, so the chips never squeeze their labels: the slots and the image buttons, then the
  // chips and which SteamGridDB game the art is from.
  auto* rows = new QVBoxLayout(bar);
  rows->setContentsMargins(16, 10, 16, 10);
  rows->setSpacing(8);
  auto* bar_layout = new QHBoxLayout();
  bar_layout->setSpacing(8);
  rows->addLayout(bar_layout);
  auto* filter_layout = new QHBoxLayout();
  filter_layout->setSpacing(8);
  rows->addLayout(filter_layout);
  slots_ = new TabRow(bar);
  slots_->setObjectName("picker_tabs");
  slots_->AddTab("cover", "Covers");
  if (!title_) {
    slots_->AddTab("hero", "Hero art");
    slots_->AddTab("logo", "Logos");
    slots_->AddTab("icon", "Icons");
  }
  connect(slots_, &TabRow::CurrentChanged, this, [this](const QString& slot) {
    if (HasChange()) emit Previewed(QString::fromStdString(slot_), QPixmap());
    Open(slot.toStdString());
    emit PickChanged(false);
  });
  bar_layout->addWidget(slots_);
  bar_layout->addStretch(1);
  // Wraps when there are more styles than fit.
  auto* chips = new QWidget(bar);
  chips_layout_ = new FlowLayout(chips, 6);
  filter_layout->addWidget(chips, 1);
  chips_ = new QButtonGroup(this);
  chips_->setExclusive(true);
  // Narrows any style down to what SteamGridDB marks adult.
  adult_chip_ = new QPushButton(chips);
  adult_chip_->setObjectName("chip");
  adult_chip_->setCheckable(true);
  adult_chip_->setCursor(Qt::PointingHandCursor);
  adult_chip_->hide();
  connect(adult_chip_, &QPushButton::toggled, this, [this](bool checked) {
    adult_only_ = checked;
    ApplyFilter(filter_, filter_style_);
  });
  chips_layout_->addWidget(adult_chip_);
  // A chip's height held from the start, so the grid doesn't move down when they arrive.
  chips->setMinimumHeight(adult_chip_->sizeHint().height());

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
  filter_layout->addWidget(match_row_, 0, Qt::AlignTop);
  auto* choose = new QPushButton("Choose image…", bar);
  choose->setToolTip("Use a PNG, JPEG or WebP of your own. You can also drop one here.");
  connect(choose, &QPushButton::clicked, this, &ArtPickerPanel::ChooseFile);
  bar_layout->addWidget(choose);
  url_button_ = new QPushButton("From URL…", bar);
  url_button_->setToolTip("Download a PNG, JPEG or WebP from the web. You can also drop a link.");
  bar_layout->addWidget(url_button_);
  url_field_ = new QLineEdit(bar);
  url_field_->setPlaceholderText("Image URL, then Enter");
  url_field_->setClearButtonEnabled(true);
  url_field_->setMinimumWidth(240);
  url_field_->hide();
  bar_layout->addWidget(url_field_);
  connect(url_button_, &QPushButton::clicked, this, [this] {
    url_button_->hide();
    url_field_->show();
    url_field_->setFocus();
  });
  connect(url_field_, &QLineEdit::returnPressed, this, [this] {
    const QUrl url = QUrl::fromUserInput(url_field_->text().trimmed());
    if (!IsWebUrl(url)) {
      emit ApplyFailed(QString::fromStdString(slot_), "Not a web address: " + url_field_->text());
      return;
    }
    UseUrl(url);
  });
  // Leaving it empty goes back to the button.
  connect(url_field_, &QLineEdit::editingFinished, this, [this] {
    if (!url_field_->text().trimmed().isEmpty()) return;
    url_field_->hide();
    url_button_->show();
  });
  network_ = new QNetworkAccessManager(this);
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
  connect(pulse_, &QVariantAnimation::valueChanged, grid_->viewport(),
          [this] { grid_->viewport()->update(); });
  connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this,
          &ArtPickerPanel::RequestVisible);
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
  // Picks None in the grid, for the host to apply like any pick.
  none_button_ = new QPushButton(message_page_);
  connect(none_button_, &QPushButton::clicked, this, [this] {
    stack_->setCurrentWidget(grid_);
    grid_->setCurrentItem(ItemFor(kNoneId));
  });
  message_layout->addWidget(none_button_, 0, Qt::AlignHCenter);
  message_layout->addStretch(1);
  stack_->addWidget(message_page_);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  // A page's ready event may have been missed while disconnected: ask for it again.
  connect(EventHub::Instance(), &EventHub::ConnectionChanged, this, [this](bool connected) {
    if (!connected || !page_loading_) return;
    page_loading_ = false;
    RequestPage();
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
    if (!std::exchange(counts_requested_, true)) RequestCounts(result);
  });
}

void ArtPickerPanel::RequestCounts(const GameMetadataResult& result) {
  if (title_ || !result.ok) return;  // a store title's picker has only covers
  for (const char* slot : {"cover", "hero", "logo", "icon"}) {
    if (slot == slot_) continue;
    const auto cached = result.metadata.candidates.find(slot);
    const std::vector<ArtCandidate> none;
    const std::vector<ArtCandidate>& candidates =
        cached == result.metadata.candidates.end() ? none : cached->second;
    own_counts_[slot] = static_cast<int>(std::ranges::count_if(
        candidates, [](const ArtCandidate& c) { return c.source != "steamgriddb"; }));
    slots_->SetCount(slot, static_cast<int>(candidates.size()));
    const std::string request = std::to_string(QRandomGenerator::global()->generate64());
    count_requests_[slot] = request;
    // game.artwork_candidates_ready brings the total; a failure leaves the cached count.
    api::FetchArtCandidatesAsync(this, id_, slot, 0, request, "", false, [](GameActionResult) {});
  }
}

void ArtPickerPanel::Populate(const GameMetadataResult& result) {
  candidates_.clear();
  active_id_.reset();
  std::string source;
  if (result.ok) {
    if (const auto found = result.metadata.candidates.find(slot_);
        found != result.metadata.candidates.end()) {
      candidates_ = found->second;
    }
    if (const auto found = result.metadata.active_candidate_ids.find(slot_);
        found != result.metadata.active_candidate_ids.end()) {
      active_id_ = found->second;
    }
    if (const auto found = result.metadata.art_sources.find(slot_);
        found != result.metadata.art_sources.end()) {
      source = found->second;
    }
  }
  if (source == "none") active_id_ = kNoneId;
  if (source == "upload") active_id_ = kUploadId;
  upload_.clear();
  thumbs_.erase({slot_, kUploadId});
  delegate_->fit = slot_ == "logo" || slot_ == "icon";
  filter_.clear();
  filter_style_.clear();
  adult_only_ = false;
  adult_chip_->blockSignals(true);
  adult_chip_->setChecked(false);
  adult_chip_->blockSignals(false);
  next_page_ = 0;
  more_pages_ = true;
  page_loading_ = false;
  griddb_total_ = -1;
  style_counts_.clear();
  adult_count_ = 0;
  counts_known_ = false;
  paged_ = {};
  query_loaded_.clear();
  preview_.reset();

  grid_->blockSignals(true);
  grid_->clear();
  AddOwnItems(source);
  for (const ArtCandidate& candidate : candidates_) AddItem(candidate);
  QListWidgetItem* current = active_id_ ? ItemFor(*active_id_) : nullptr;
  if (current != nullptr) grid_->setCurrentItem(current);
  grid_->blockSignals(false);
  pick_ = active_id_;

  UpdateTitle();
  RebuildChips();
  UpdateGridSize();
  if (candidates_.empty() && !(active_id_ && IsOwnItem(*active_id_))) {
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

void ArtPickerPanel::AddOwnItems(const std::string& source) {
  if (source == "upload") {
    auto* yours = new QListWidgetItem(grid_);
    yours->setData(kIdRole, QVariant::fromValue<qlonglong>(kUploadId));
    yours->setData(kStyleRole, QString("Yours"));
    yours->setData(kCurrentRole, true);
    yours->setData(kStateRole, kLoading);
    yours->setToolTip("Your image");
    const std::string slot = slot_;
    api::GetArtworkImageAsync(
        this, id_, slot,
        [this, slot](QImage image) {
          // Gone, or replaced by a newer image of the user's, meanwhile.
          QListWidgetItem* item = slot == slot_ && upload_.empty() ? ItemFor(kUploadId) : nullptr;
          if (item == nullptr) return;
          if (image.isNull()) {
            item->setData(kStateRole, kFailed);
            return;
          }
          item->setToolTip(QString("Your image · %1×%2").arg(image.width()).arg(image.height()));
          const QPixmap pixmap = QPixmap::fromImage(std::move(image));
          thumbs_[{slot, kUploadId}] = pixmap;
          item->setData(Qt::DecorationRole, pixmap);
          item->setData(kStateRole, kReady);
        },
        QSize(1920, 1920));
  }
  auto* none = new QListWidgetItem(grid_);
  none->setData(kIdRole, QVariant::fromValue<qlonglong>(kNoneId));
  none->setData(kStyleRole, QString("None"));
  none->setData(kCurrentRole, source == "none");
  none->setData(kStateRole, kReady);
  none->setData(kTextRole, QString("None"));
  none->setToolTip("No image: the placeholder, which stays through refreshes");
}

QListWidgetItem* ArtPickerPanel::AddItem(const ArtCandidate& candidate) {
  auto* item = new QListWidgetItem();
  if (QListWidgetItem* placeholder = ItemFor(kPlaceholderId)) {
    const int row = grid_->row(placeholder);
    delete grid_->takeItem(row);
    grid_->insertItem(row, item);
  } else {
    grid_->addItem(item);
  }
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
  item->setHidden(!PassesFilter(item));
  return item;
}

bool ArtPickerPanel::PassesFilter(const QListWidgetItem* item) const {
  const std::int64_t id = item->data(kIdRole).toLongLong();
  // The user's own image and None have no style, and stay in sight.
  if (IsOwnItem(id)) return true;
  // SteamGridDB's art still to come is of whatever its pages are asked for.
  if (id == kPlaceholderId) return filter_.isEmpty() || !filter_style_.empty();
  if (!filter_.isEmpty() && item->data(kStyleRole).toString() != filter_) return false;
  return !adult_only_ || item->data(kNsfwRole).toBool();
}

void ArtPickerPanel::UpdateTitle() {
  // The store's own art plus however many SteamGridDB has, once known.
  int count = static_cast<int>(candidates_.size());
  if (griddb_total_ >= 0) {
    count = griddb_total_ +
            static_cast<int>(std::ranges::count_if(
                candidates_, [](const ArtCandidate& c) { return c.source != "steamgriddb"; }));
  }
  slots_->SetCount(QString::fromStdString(slot_), count);
}

void ArtPickerPanel::RequestPage() {
  if (!more_pages_ || page_loading_) return;
  page_loading_ = true;
  const std::string slot = slot_;
  page_request_ = std::to_string(QRandomGenerator::global()->generate64());
  paged_ = {filter_style_, adult_only_};
  api::FetchArtCandidatesAsync(this, id_, slot, next_page_, page_request_, filter_style_,
                               adult_only_, [this, slot](GameActionResult result) {
                                 // game.artwork_candidates_ready follows
                                 if (result.ok || slot != slot_) return;
                                 page_loading_ = false;
                                 more_pages_ = false;
                                 SetPlaceholders(0);
                                 RebuildChips();
                                 if (candidates_.empty()) ShowNothingFound();
                               });
}

void ArtPickerPanel::RestartPaging() {
  grid_->blockSignals(true);
  // SteamGridDB's art from the earlier pages goes, but for the one in use and the pick.
  for (int i = grid_->count() - 1; i >= 0; --i) {
    const std::int64_t id = grid_->item(i)->data(kIdRole).toLongLong();
    const auto candidate = std::ranges::find(candidates_, id, &ArtCandidate::id);
    const bool griddb = candidate != candidates_.end() && !IsLocal(*candidate);
    if (id != kPlaceholderId && (!griddb || id == active_id_ || id == pick_)) continue;
    if (griddb) candidates_.erase(candidate);
    delete grid_->takeItem(i);
  }
  grid_->blockSignals(false);
  next_page_ = 0;
  more_pages_ = true;
  page_loading_ = false;  // an answer for the earlier pages no longer matches page_request_
  query_loaded_.clear();
  // The grid at its length from the counts already, when they say.
  int expected = -1;
  if (!adult_only_ && filter_style_.empty()) {
    expected = griddb_total_;
  } else if (!adult_only_) {
    const auto found =
        std::ranges::find(style_counts_, filter_style_, &std::pair<std::string, int>::first);
    expected = found != style_counts_.end() ? found->second : -1;
  } else if (filter_style_.empty()) {
    expected = adult_count_;
  }
  SetPlaceholders(std::max(0, expected));
  grid_->verticalScrollBar()->setValue(0);
  RequestPage();
}

void ArtPickerPanel::SetPlaceholders(int count) {
  int have = 0;
  for (int i = 0; i < grid_->count(); ++i) {
    if (grid_->item(i)->data(kIdRole).toLongLong() == kPlaceholderId) ++have;
  }
  grid_->blockSignals(true);
  for (int i = grid_->count() - 1; i >= 0 && have > count; --i) {
    if (grid_->item(i)->data(kIdRole).toLongLong() != kPlaceholderId) continue;
    delete grid_->takeItem(i);
    --have;
  }
  for (; have < count; ++have) {
    auto* item = new QListWidgetItem(grid_);
    item->setData(kIdRole, QVariant::fromValue<qlonglong>(kPlaceholderId));
    item->setData(kStateRole, kIdle);
    item->setFlags(Qt::NoItemFlags);
    item->setHidden(!PassesFilter(item));
  }
  grid_->blockSignals(false);
}

void ArtPickerPanel::ShowNothingFound() {
  // The grid still holds what's in use when that's None or the user's own image.
  if (active_id_ && IsOwnItem(*active_id_)) {
    stack_->setCurrentWidget(grid_);
    return;
  }
  ShowMessage(QString("No other %1 found for this game.").arg(SlotNoun(slot_)), true);
}

void ArtPickerPanel::ShowPage(const ArtCandidatesEvent& event) {
  page_loading_ = false;
  if (!event.error.empty()) {
    // No key or no match: what the fetch cached is all there is.
    more_pages_ = false;
    SetPlaceholders(0);
    RebuildChips();
    if (candidates_.empty()) ShowNothingFound();
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
      if (candidate == candidates_.end() || IsLocal(*candidate)) continue;
      if (active_id_ == id && !live.contains(id)) continue;
      if (pick_ == id && !live.contains(id)) {
        pick_ = active_id_;  // and highlighted, or taking the item moves the highlight to a
                             // neighbour unseen
        grid_->setCurrentItem(active_id_ ? ItemFor(*active_id_) : nullptr);
      }
      candidates_.erase(candidate);
      delete grid_->takeItem(i);
    }
  }
  for (const ArtCandidate& candidate : event.candidates) {
    query_loaded_.insert(candidate.id);
    if (std::ranges::find(candidates_, candidate.id, &ArtCandidate::id) != candidates_.end())
      continue;
    candidates_.push_back(candidate);
    QListWidgetItem* item = AddItem(candidate);
    if (pick_ == candidate.id) grid_->setCurrentItem(item);
  }
  grid_->blockSignals(false);
  if (event.styles) {
    style_counts_ = *event.styles;
    std::ranges::stable_sort(style_counts_, std::greater{}, &std::pair<std::string, int>::second);
    adult_count_ = event.adult;
    counts_known_ = true;
  }
  if (paged_ == std::pair<std::string, bool>{}) griddb_total_ = event.total;
  next_page_ = event.page + 1;
  const int loaded = static_cast<int>(query_loaded_.size());
  more_pages_ = !event.candidates.empty() && loaded < event.total;
  SetPlaceholders(more_pages_ ? event.total - loaded : 0);
  UpdateTitle();
  RebuildChips();
  if (candidates_.empty()) {
    ShowNothingFound();
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
  // Until SteamGridDB's counts arrive with its first page; counting what's loaded would be wrong.
  if (!counts_known_ && more_pages_) {
    adult_chip_->hide();
    return;
  }
  // Art from elsewhere is counted here; SteamGridDB's own list from its counts, when they came.
  struct Chip {
    QString label;
    std::string style;  // SteamGridDB's, for its pages
    int count = 0;
  };
  std::vector<Chip> styles;
  for (const ArtCandidate& candidate : candidates_) {
    if (counts_known_ && !IsLocal(candidate)) continue;
    const QString label = StyleLabel(candidate);
    auto found = std::ranges::find(styles, label, &Chip::label);
    if (found == styles.end()) {
      styles.push_back({label, {}, 1});
    } else {
      ++found->count;
    }
  }
  for (const auto& [style, count] : style_counts_)
    styles.push_back({StyleName(style), style, count});
  const int adult = counts_known_
                        ? adult_count_
                        : static_cast<int>(std::ranges::count_if(candidates_, &ArtCandidate::nsfw));
  adult_chip_->setText(QString("Adult  %1").arg(adult));
  adult_chip_->setVisible(adult > 0);
  if (adult == 0 && adult_only_) adult_chip_->setChecked(false);  // shows the rest again
  // One style is nothing to filter by.
  if (styles.size() < 2) return;
  int all = 0;
  for (const Chip& chip : styles) all += chip.count;
  styles.insert(styles.begin(), {QString(), {}, all});
  for (const auto& [label, style, count] : styles) {
    auto* chip = new QPushButton(QString("%1  %2").arg(label.isEmpty() ? "All" : label).arg(count),
                                 adult_chip_->parentWidget());
    chip->setObjectName("chip");
    chip->setCheckable(true);
    chip->setChecked(label == filter_);
    chip->setCursor(Qt::PointingHandCursor);
    connect(chip, &QPushButton::clicked, this, [this, label, style] { ApplyFilter(label, style); });
    chips_->addButton(chip);
    chips_layout_->addWidget(chip);
  }
  chips_layout_->Move(adult_chip_, chips_layout_->count() - 1);
}

void ArtPickerPanel::ApplyFilter(const QString& label, const std::string& style) {
  filter_ = label;
  filter_style_ = style;
  for (int i = 0; i < grid_->count(); ++i) grid_->item(i)->setHidden(!PassesFilter(grid_->item(i)));
  grid_->verticalScrollBar()->setValue(0);
  // Art from elsewhere is all loaded already; SteamGridDB's is asked for by style.
  const bool local = !label.isEmpty() && style.empty();
  if (!local && paged_ != std::pair{filter_style_, adult_only_}) RestartPaging();
  // The grid lays the rest out again only once control returns to it.
  QTimer::singleShot(0, this, &ArtPickerPanel::RequestVisible);
}

void ArtPickerPanel::RequestVisible() {
  if (candidates_.empty() || stack_->currentWidget() != grid_) return;
  // A screen ahead, so scrolling finds previews already there.
  const QRect ahead = grid_->viewport()->rect().adjusted(0, 0, 0, grid_->viewport()->height());
  std::vector<std::int64_t> ids;
  QListWidgetItem* last_loaded = nullptr;  // the last shown that isn't a placeholder
  bool placeholder_near = false;
  for (int i = 0; i < grid_->count(); ++i) {
    QListWidgetItem* item = grid_->item(i);
    if (item->isHidden()) continue;
    const std::int64_t id = item->data(kIdRole).toLongLong();
    if (id == kPlaceholderId) {
      // Within a screen, or scrolled past.
      placeholder_near = placeholder_near || grid_->visualItemRect(item).top() < ahead.bottom();
      continue;
    }
    last_loaded = item;
    if (ids.size() >= kMaxBatch || item->data(kStateRole).toInt() != kIdle) continue;
    if (!grid_->visualItemRect(item).intersects(ahead)) continue;
    item->setData(kStateRole, kLoading);
    ids.push_back(id);
  }
  // The end of what's loaded is within a screen: SteamGridDB's next page.
  if (placeholder_near || last_loaded == nullptr ||
      grid_->visualItemRect(last_loaded).intersects(ahead))
    RequestPage();
  if (ids.empty()) return;
  UpdatePulse();
  const std::string slot = slot_;
  api::FetchArtThumbsAsync(this, id_, slot, ids, [this, slot, ids](GameActionResult result) {
    if (!result.ok && slot == slot_) MarkFailed(ids);
  });
}

void ArtPickerPanel::ShowThumbs(const std::string& slot,
                                const std::vector<std::pair<std::int64_t, QImage>>& images) {
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
    if (QListWidgetItem* item = ItemFor(candidate_id);
        item != nullptr && item->data(kStateRole).toInt() != kReady) {
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
  const std::int64_t pick = *pick_;
  if (preview_ && preview_->first == pick) {
    emit Previewed(slot, preview_->second);
    return;
  }
  // Not loaded yet: ShowThumbs calls back here when it is.
  const auto found = thumbs_.find({slot_, pick});
  if (found != thumbs_.end()) {
    emit Previewed(slot, found->second);
  } else if (pick == kNoneId) {
    emit Previewed(slot, QPixmap());  // the art in use, rather than an earlier pick's
  }
  if (IsOwnItem(pick) || found == thumbs_.end()) return;
  // The grid's copy is the size of its cell; the host shows the pick larger.
  api::GetArtThumbsAsync(
      this, id_, slot_, {pick},
      [this, slot = slot_, pick](std::vector<std::pair<std::int64_t, QImage>> images) {
        if (images.empty() || slot != slot_ || pick_ != pick) return;
        preview_.emplace(pick, QPixmap::fromImage(std::move(images.front().second)));
        emit Previewed(QString::fromStdString(slot), preview_->second);
      });
}

bool ArtPickerPanel::HasChange() const {
  return pick_.has_value() && pick_ != active_id_;
}

bool ArtPickerPanel::PicksNone() const {
  return pick_ == kNoneId;
}

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
  if (!HasChange() || applying_) return;
  const std::string slot = slot_;
  const std::int64_t candidate_id = *pick_;
  applying_ = {slot, candidate_id};
  auto done = [this, slot](ArtworkSelectResult result) {
    if (result.ok) return;  // game.artwork_selected follows
    applying_.reset();
    emit ApplyFailed(QString::fromStdString(slot), error_help::Describe(result.error));
  };
  if (candidate_id == kUploadId && upload_.empty()) {
    // The image saved earlier, shown from mirad's copy: sent back as it was read.
    const auto found = thumbs_.find({slot, kUploadId});
    if (found == thumbs_.end()) {
      applying_.reset();
      return;
    }
    async::Run<std::string>(
        this,
        [image = found->second.toImage()] {
          QByteArray png;
          QBuffer buffer(&png);
          buffer.open(QIODevice::WriteOnly);
          image.save(&buffer, "PNG");
          return png.toStdString();
        },
        [this, slot, done = std::move(done)](std::string bytes) mutable {
          api::UploadArtworkAsync(this, id_, slot, std::move(bytes), std::move(done));
        });
  } else if (candidate_id == kUploadId) {
    api::UploadArtworkAsync(this, id_, slot, upload_, std::move(done));
  } else if (candidate_id == kNoneId) {
    api::RemoveArtworkAsync(this, id_, slot, std::move(done));
  } else {
    api::SelectArtworkAsync(this, id_, slot, candidate_id, std::move(done));
  }
}

void ArtPickerPanel::ChooseFile() {
  const QString path = QFileDialog::getOpenFileName(
      this, "Choose an image", QString(), "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)");
  if (!path.isEmpty()) UseFile(path);
}

void ArtPickerPanel::dragEnterEvent(QDragEnterEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.size() == 1 && (IsImageUrl(urls.front()) || IsWebUrl(urls.front())))
    event->acceptProposedAction();
}

void ArtPickerPanel::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.size() != 1) return;
  const QUrl& url = urls.front();
  if (IsImageUrl(url)) {
    event->acceptProposedAction();
    UseFile(url.toLocalFile());
  } else if (IsWebUrl(url)) {
    event->acceptProposedAction();
    UseUrl(url);
  }
}

namespace {

struct LoadedFile {
  QString error;
  std::string upload;
  QImage preview;
  QSize size;
};

// The format of an image Qt has no plugin for, by its first bytes. Sites serve these under a
// .png or .jpg name.
QString UnreadableFormat(const QByteArray& bytes) {
  if (bytes.mid(4, 4) == "ftyp") {
    const QByteArray brand = bytes.mid(8, 4);
    if (brand == "avif" || brand == "avis") return "AVIF";
    if (brand == "heic" || brand == "heix" || brand == "mif1") return "HEIF";
  }
  if (bytes.startsWith("\xFF\x0A") || bytes.startsWith(QByteArray("\0\0\0\x0CJXL ", 8)))
    return "JPEG XL";
  return {};
}

// `bytes` as mirad takes them, plus a preview. Slow for a big image: run it off the UI thread.
LoadedFile Decode(const QByteArray& bytes, const QString& name) {
  QBuffer source;
  source.setData(bytes);
  source.open(QIODevice::ReadOnly);
  QImageReader reader(&source);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull()) {
    if (const QString format = UnreadableFormat(bytes); !format.isEmpty())
      return {.error = QString("Could not read %1: it is %2, whatever its name says, and this "
                               "build of Mira can't read %2 images.")
                           .arg(name, format)};
    return {.error = "Could not read " + name + ": " + reader.errorString()};
  }
  LoadedFile loaded{.size = image.size()};
  // mirad reads PNG and JPEG, and ignores EXIF rotation; anything else goes as PNG, upright.
  if ((bytes.startsWith("\x89PNG") || bytes.startsWith("\xFF\xD8\xFF")) &&
      reader.transformation() == QImageIOHandler::TransformationNone) {
    loaded.upload = bytes.toStdString();
  } else {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {.error = "Could not convert " + name + " to PNG"};
    loaded.upload = png.toStdString();
  }
  loaded.preview = image.width() > 1920 || image.height() > 1920
                       ? image.scaled(1920, 1920, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                       : image;
  return loaded;
}

}  // namespace

void ArtPickerPanel::UseFile(const QString& path) {
  auto load = [path]() -> LoadedFile {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {.error = "Could not open " + path + ": " + file.errorString()};
    return Decode(file.readAll(), path);
  };
  async::Run<LoadedFile>(
      this, std::move(load),
      [this, name = QFileInfo(path).fileName(), slot = slot_](LoadedFile loaded) {
        if (!loaded.error.isEmpty()) {
          emit ApplyFailed(QString::fromStdString(slot), loaded.error);
          return;
        }
        if (slot != slot_) return;  // moved to another slot meanwhile
        upload_ = std::move(loaded.upload);
        ShowUpload(name, QPixmap::fromImage(std::move(loaded.preview)), loaded.size);
      });
}

void ArtPickerPanel::UseBytes(QByteArray bytes, const QString& name) {
  async::Run<LoadedFile>(
      this, [bytes = std::move(bytes), name] { return Decode(bytes, name); },
      [this, name, slot = slot_](LoadedFile loaded) {
        if (!loaded.error.isEmpty()) {
          emit ApplyFailed(QString::fromStdString(slot), loaded.error);
          return;
        }
        if (slot != slot_) return;
        upload_ = std::move(loaded.upload);
        ShowUpload(name, QPixmap::fromImage(std::move(loaded.preview)), loaded.size);
      });
}

void ArtPickerPanel::UseUrl(const QUrl& url) {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(30'000);
  QNetworkReply* reply = network_->get(request);
  url_field_->setEnabled(false);
  const QString slot = QString::fromStdString(slot_);
  // Stops a download that says, or turns out, larger than any art needs.
  auto too_big = [reply](qint64 size) {
    if (size <= kMaxDownload) return false;
    reply->setProperty("too_big", true);
    reply->abort();
    return true;
  };
  connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, too_big] {
    too_big(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
  });
  connect(reply, &QNetworkReply::downloadProgress, reply,
          [too_big](qint64 received, qint64) { too_big(received); });
  connect(reply, &QNetworkReply::finished, this, [this, reply, url, slot] {
    reply->deleteLater();
    url_field_->setEnabled(true);
    // Disabling it dropped its focus; back to it, to fix the address after a failure.
    url_field_->setFocus();
    if (reply->property("too_big").toBool()) {
      emit ApplyFailed(slot, url.toDisplayString() + " is larger than 32 MB.");
      return;
    }
    const QString where = url.toDisplayString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // The site answered and refused: many serve their images only to their own pages.
    if (status == 401 || status == 403 || status == 429 || status == 451) {
      emit ApplyFailed(slot, QString("%1 doesn't allow downloading its images from other apps "
                                     "(HTTP %2). Save the image from a browser and use Choose "
                                     "image… instead.")
                                 .arg(url.host())
                                 .arg(status));
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      emit ApplyFailed(slot, "Could not download " + where + ": " + reply->errorString());
      return;
    }
    if (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/html")) {
      emit ApplyFailed(slot, where +
                                 " is a web page, not an image. Copy the image's own address "
                                 "(right-click the image) instead.");
      return;
    }
    if (slot.toStdString() != slot_) return;
    url_field_->clear();
    url_field_->hide();
    url_button_->show();
    const QString name = url.fileName().isEmpty() ? url.host() : url.fileName();
    UseBytes(reply->readAll(), name);
  });
}

void ArtPickerPanel::ShowUpload(const QString& name, const QPixmap& pixmap, QSize size) {
  thumbs_[{slot_, kUploadId}] = pixmap;

  QListWidgetItem* item = ItemFor(kUploadId);
  if (item == nullptr) {
    item = new QListWidgetItem();
    item->setData(kIdRole, QVariant::fromValue<qlonglong>(kUploadId));
    item->setData(kStyleRole, QString("Yours"));
    grid_->insertItem(0, item);
  }
  // A new image in place of the one in use: nothing in the grid is in use now.
  if (active_id_ == kUploadId) active_id_.reset();
  item->setData(Qt::DecorationRole, pixmap);
  item->setData(kStateRole, kReady);
  item->setData(kCurrentRole, false);
  item->setToolTip(name + QString(" · %1×%2").arg(size.width()).arg(size.height()));
  item->setHidden(false);
  stack_->setCurrentWidget(grid_);
  grid_->setCurrentItem(item);  // picks it, through currentItemChanged
  grid_->scrollToItem(item);
  if (pick_ == kUploadId) Pick(item);  // the same item again, with a new image
}

void ArtPickerPanel::UpdateGridSize() {
  // Sized as if the scroll bar were always there, so it showing up doesn't
  // change the column count and lay everything out again.
  const int scroll_bar = grid_->verticalScrollBar()->sizeHint().width();
  const int width = grid_->width() - 20 - scroll_bar;  // less the grid's padding
  // SteamGridDB heroes are 96:31, covers 2:3; logos vary, so a wide box, and icons are square.
  const bool hero = slot_ == "hero";
  const bool logo = slot_ == "logo";
  const bool icon = slot_ == "icon";
  const int columns = std::max(2, width / (hero ? 230 : logo ? 200 : icon ? 96 : 118));
  const int cell = std::max(40, width / columns);
  const int art = cell - 2 * kInset;
  const int height = qRound(art * (hero   ? 31.0 / 96.0
                                   : logo ? 0.5
                                   : icon ? 1.0
                                          : 1.5)) +
                     2 * kInset;
  grid_->setGridSize(QSize(cell, height));
}

void ArtPickerPanel::UpdatePulse() {
  bool loading = false;
  for (int i = 0; i < grid_->count() && !loading; ++i)
    loading = grid_->item(i)->data(kStateRole).toInt() == kLoading;
  if (loading && pulse_->state() != QAbstractAnimation::Running) pulse_->start();
  if (!loading) pulse_->stop();
}

void ArtPickerPanel::ShowMessage(const QString& text, bool offer_fetch) {
  message_->setText(text);
  // A title has no game to refresh; its match row refetches instead.
  fetch_button_->setVisible(offer_fetch && !title_);
  none_button_->setText("Use no " + SlotSingular(slot_));
  none_button_->setVisible(offer_fetch && active_id_ != kNoneId);
  stack_->setCurrentWidget(message_page_);
}

void ArtPickerPanel::LoadMatches(const QString& query) {
  match_->setEnabled(false);
  api::GetGriddbMatchesAsync(
      this, id_, query.toStdString(), [this, query](GriddbMatchesResult result) {
        match_->setEnabled(true);
        match_search_->hide();
        match_->show();
        // No key, or SteamGridDB unreachable: nothing to choose between.
        if (!result.ok) {
          if (!query.isEmpty())
            ShowMessage("Could not search SteamGridDB: " + error_help::Describe(result.error),
                        false);
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
    if (!events::ParseArtCandidatesEvent(data, &event) || event.id != id_) return;
    if (const auto asked = count_requests_.find(event.slot);
        asked != count_requests_.end() && asked->second == event.request) {
      count_requests_.erase(asked);
      if (event.error.empty())
        slots_->SetCount(QString::fromStdString(event.slot), event.total + own_counts_[event.slot]);
      return;
    }
    if (event.slot != slot_) return;
    if (!page_loading_ || event.request != page_request_)
      return;  // an earlier Open()'s, or a replay
    ShowPage(event);
    return;
  }

  if (type == "game.artwork_thumbs_ready") {
    ArtThumbsEvent event;
    if (!events::ParseArtThumbsEvent(data, &event) || event.id != id_ || event.slot != slot_)
      return;
    MarkFailed(event.failed);
    if (event.ready.empty()) return;
    const std::string slot = event.slot;
    const std::vector<std::int64_t> ready = event.ready;
    // Decoded at about the cells' size: a few hundred whole previews would hold over 100 MB. A
    // cover or hero fills its cell, so it's kept large enough to crop.
    const qreal fill = delegate_->fit ? 1.0 : 1.5;
    const QSize cell = grid_->gridSize() - QSize(2 * kInset, 2 * kInset);
    api::GetArtThumbsAsync(
        this, id_, slot, ready,
        [this, slot, ready](std::vector<std::pair<std::int64_t, QImage>> images) {
          ShowThumbs(slot, images);
          if (slot != slot_) return;
          std::vector<std::int64_t> missing;
          for (const std::int64_t candidate_id : ready) {
            if (!thumbs_.contains({slot, candidate_id})) missing.push_back(candidate_id);
          }
          MarkFailed(missing);
        },
        (QSizeF(cell) * grid_->devicePixelRatioF() * fill).toSize());
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
