#include "TagsPage.h"

#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>

#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/JsonMapping.h"
#include "../client/api/Config.h"
#include "../client/api/Tags.h"
#include "../library/ArtworkStore.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "TagPicker.h"

namespace mira_gui {
namespace {

constexpr int kCountWidth = 64;

QString Games(std::size_t count) {
  return count == 1 ? QString("1 game") : QString("%1 games").arg(count);
}

QString FoldersMove(int count) {
  return count == 1 ? QString("1 game folder moves") : QString("%1 game folders move").arg(count);
}

SettingRow* NoteRow(QWidget* parent) {
  auto* row = new SettingRow(QString(), {}, parent);
  row->Label()->setProperty("role", "subtle");
  return row;
}

QLabel* CountLabel(QWidget* parent) {
  auto* count = new QLabel(parent);
  count->setProperty("role", "subtle");
  count->setFixedWidth(kCountWidth);
  count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  return count;
}

QToolButton* MoreButton(QWidget* parent) {
  auto* more = new QToolButton(parent);
  more->setAutoRaise(true);
  icons::Follow(more, icons::Glyph::More, &theme::Tokens::text_muted);
  return more;
}

QLabel* ColumnHeading(const QString& text, int width, Qt::Alignment align, QWidget* parent) {
  auto* heading = new QLabel(text.toUpper(), parent);
  heading->setProperty("role", "group_heading");
  heading->setFixedWidth(width);
  heading->setAlignment(align | Qt::AlignVCenter);
  return heading;
}

// Column headings over a card's rows, built as a row so they line up with the rows' controls.
SettingRow* HeadingsRow(QWidget* parent) {
  auto* row = new SettingRow(QString(), {}, parent);
  row->Label()->setMinimumHeight(0);
  const QMargins margins = row->layout()->contentsMargins();
  row->layout()->setContentsMargins(margins.left(), 0, margins.right(), 4);
  return row;
}

void KeepSpaceWhenHidden(QWidget* widget) {
  QSizePolicy policy = widget->sizePolicy();
  policy.setRetainSizeWhenHidden(true);
  widget->setSizePolicy(policy);
}

QString FolderName(const std::string& root) { return QDir(QString::fromStdString(root)).dirName(); }

// The same folder however a setting spells it: "~/Mira/Games", "/home/me/Mira/Games/".
bool SameFolder(const std::string& a, const std::string& b) {
  const auto expanded = [](const std::string& path) {
    QString text = QString::fromStdString(path);
    if (text == "~" || text.startsWith("~/")) text = QDir::homePath() + text.mid(1);
    return QDir::cleanPath(text);
  };
  return expanded(a) == expanded(b);
}

std::vector<std::string> With(std::vector<std::string> list, const std::string& item, bool in) {
  std::erase_if(list, [&](const std::string& entry) { return SameTag(entry, item); });
  if (in) list.push_back(item);
  return list;
}

}  // namespace

// Rows in as many equal columns as fit, read across, with lines between them like a card's.
class RowColumns : public QWidget {
public:
  explicit RowColumns(QWidget* parent) : QWidget(parent) {}

  // Takes `row`, or moves it, to `index` in the reading order. Relayout places it.
  void Place(QWidget* row, int index) {
    rows_.removeAll(row);
    if (row->parent() != this) row->setParent(this);
    rows_.insert(std::clamp(index, 0, static_cast<int>(rows_.size())), row);
  }
  void Remove(QWidget* row) {
    rows_.removeAll(row);
    delete row;
  }
  // After rows were placed, removed, shown or hidden.
  void Relayout() {
    updateGeometry();
    Lay();
    update();
  }

  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override {
    const int shown = static_cast<int>(Shown().size());
    const int columns = Columns(width);
    return (shown + columns - 1) / columns * RowHeight();
  }
  QSize sizeHint() const override { return {kMinColumn, heightForWidth(width() > 0 ? width() : kMinColumn)}; }
  QSize minimumSizeHint() const override { return {kMinColumn, 0}; }

protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    Lay();
  }
  void paintEvent(QPaintEvent*) override {
    const QList<QWidget*> shown = Shown();
    if (shown.isEmpty()) return;
    QPainter painter(this);
    QColor line = theme::Current().border;
    line.setAlpha(150);
    painter.setPen(line);
    const int columns = Columns(width());
    const int lines = (static_cast<int>(shown.size()) + columns - 1) / columns;
    for (int i = 1; i < lines; ++i) painter.drawLine(1, i * RowHeight(), width() - 2, i * RowHeight());
    for (int c = 1; c < columns && c < shown.size(); ++c) {
      const int x = shown[c]->x();
      painter.drawLine(x, 6, x, lines * RowHeight() - 6);
    }
  }

private:
  static constexpr int kMinColumn = 250;

  QList<QWidget*> Shown() const {
    QList<QWidget*> shown;
    for (QWidget* row : rows_) {
      if (!row->isHidden()) shown.append(row);
    }
    return shown;
  }
  static int Columns(int width) { return std::max(1, width / kMinColumn); }
  int RowHeight() const {
    int height = 0;
    for (const QWidget* row : rows_) height = std::max(height, row->sizeHint().height());
    return height;
  }
  void Lay() {
    const QList<QWidget*> shown = Shown();
    const int columns = Columns(width());
    const int height = RowHeight();
    for (int i = 0; i < shown.size(); ++i) {
      const int column = i % columns;
      const int left = width() * column / columns;
      const int right = width() * (column + 1) / columns;
      shown[i]->setGeometry(left, i / columns * height, right - left, height);
    }
  }

  QList<QWidget*> rows_;
};

namespace {

// Where `games` sits between two games (the fewest listed until the one-game tags are shown) and
// `most`, from 0 to 1, on a log scale so the many small counts spread out.
double Share(std::size_t games, std::size_t most) {
  if (most <= 2) return 1.0;
  const double t = std::log(static_cast<double>(games) / 2) / std::log(static_cast<double>(most) / 2);
  return std::clamp(t, 0.0, 1.0);
}

// WCAG's contrast ratio between two opaque colors.
double Contrast(const QColor& a, const QColor& b) {
  const auto luminance = [](const QColor& color) {
    const auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
  };
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace

// A Steam tag's count, in an accent pill that's stronger the more games have the tag.
class CountPill : public QWidget {
public:
  explicit CountPill(QWidget* parent) : QWidget(parent) {}

  void Set(std::size_t games, std::size_t most) {
    games_ = games;
    most_ = most;
    setToolTip(Games(games));
    updateGeometry();
    update();
  }

  QSize sizeHint() const override {
    const QFontMetrics metrics(Font());
    return {std::max(24, metrics.horizontalAdvance(QString::number(games_)) + 14), metrics.height() + 2};
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(0, (height() - sizeHint().height()) / 2.0, 0,
                                               -(height() - sizeHint().height()) / 2.0);
    const theme::Tokens& t = theme::Current();
    // The accent mixed into the card's surface, opaque, so the text can be picked by its contrast.
    const double strength = 0.12 + 0.88 * Share(games_, most_);
    const auto mix = [&](int surface, int accent) {
      return static_cast<int>(std::lround(surface + (accent - surface) * strength));
    };
    const QColor fill(mix(t.surface.red(), t.accent.red()), mix(t.surface.green(), t.accent.green()),
                      mix(t.surface.blue(), t.accent.blue()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(box, box.height() / 2, box.height() / 2);
    painter.setPen(Contrast(fill, t.on_accent) >= Contrast(fill, t.text) ? t.on_accent : t.text);
    painter.setFont(Font());
    painter.drawText(box, Qt::AlignCenter, QString::number(games_));
  }

private:
  QFont Font() const {
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.88);
    small.setBold(true);
    return small;
  }

  std::size_t games_ = 0;
  std::size_t most_ = 1;
};

// One of From Steam's rows; its plus button fills while the pointer is anywhere on it.
class SteamTagRow : public QWidget {
public:
  explicit SteamTagRow(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_Hover, true); }

protected:
  void enterEvent(QEnterEvent* event) override {
    QWidget::enterEvent(event);
    update();
  }
  void leaveEvent(QEvent* event) override {
    QWidget::leaveEvent(event);
    update();
  }
};

namespace {

// A round plus, filled with the accent while its row is under the pointer.
class PlusButton : public QAbstractButton {
public:
  explicit PlusButton(QWidget* parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(24, 24);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& t = theme::Current();
    const bool lit = isDown() || (parentWidget() != nullptr && parentWidget()->underMouse()) || hasFocus();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(lit ? t.accent : t.border);
    painter.setBrush(lit ? t.accent : t.surface);
    painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    QPen plus(lit ? t.on_accent : t.accent, 2, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(plus);
    const QPointF c = QRectF(rect()).center();
    painter.drawLine(c + QPointF(-4.5, 0), c + QPointF(4.5, 0));
    painter.drawLine(c + QPointF(0, -4.5), c + QPointF(0, 4.5));
  }
};


// Esc in a field calls `cancel` instead of reaching the window's own Esc.
class EscapeCancels : public QObject {
public:
  EscapeCancels(QObject* parent, std::function<void()> cancel) : QObject(parent), cancel_(std::move(cancel)) {}

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if ((event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) ||
        static_cast<QKeyEvent*>(event)->key() != Qt::Key_Escape) {
      return QObject::eventFilter(watched, event);
    }
    event->accept();
    if (event->type() == QEvent::KeyPress) cancel_();
    return true;
  }

private:
  std::function<void()> cancel_;
};

// Events after which the tags or their counts may say something else.
bool ChangesTags(const std::string& type) {
  return type == "games.updated" || type == "game.updated" || type == "game.added" || type == "game.removed" ||
         type == "games.removed" || type == "game.metadata_ready" || type == "config.changed";
}

}  // namespace

TagsPage::TagsPage(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  setObjectName("tags_page");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  stack_ = new QStackedWidget(this);
  outer->addWidget(stack_);

  overview_ = new QWidget(stack_);
  overview_->setObjectName("settings_canvas");
  auto* overview_layout = new QVBoxLayout(overview_);
  overview_layout->setContentsMargins(32, 22, 32, 24);
  overview_layout->setSpacing(12);
  auto* title = new QLabel("Tags", overview_);
  title->setObjectName("page_title");
  overview_layout->addWidget(title);
  grid_ = new QGridLayout();
  grid_->setSpacing(16);
  overview_layout->addLayout(grid_, /*stretch=*/1);

  const int more_width = std::unique_ptr<QToolButton>(MoreButton(nullptr))->sizeHint().width();

  mine_ = new SettingsCard("Your tags", overview_);
  auto* add = new QPushButton("New tag", mine_);
  add->setObjectName("text_button");
  icons::Follow(add, icons::Glyph::Plus);
  connect(add, &QPushButton::clicked, this, [this] { OpenPicker(QString(), /*existing=*/false); });
  mine_->Header()->addWidget(add);
  mine_->SetScrollable();
  auto* mine_headings = HeadingsRow(mine_);
  mine_headings->AddControl(ColumnHeading("Games", kCountWidth, Qt::AlignRight, mine_headings));
  auto* folder_heading = ColumnHeading("Folder", 0, Qt::AlignHCenter, mine_headings);
  folder_heading->ensurePolished();  // its style's font, letter spacing included, sets its width
  folder_heading->setMinimumWidth(0);
  folder_width_ = std::max(folder_heading->sizeHint().width(), Switch().sizeHint().width()) + 4;
  folder_heading->setFixedWidth(folder_width_);
  mine_headings->AddControl(folder_heading);
  auto* more_space = new QWidget(mine_headings);
  more_space->setFixedWidth(more_width);
  mine_headings->AddControl(more_space);
  mine_->SetPinnedRow(mine_headings);
  mine_note_ = NoteRow(mine_);
  mine_note_->Label()->setText("No tags yet. Add one from Steam's, or a new one.");
  mine_->AddRow(mine_note_);
  connect(mine_, &SettingsCard::RowsReordered, this, &TagsPage::FoldersReordered);

  settings_card_ = new SettingsCard("Tag settings", overview_);

  left_ = new QWidget(overview_);
  auto* left_layout = new QVBoxLayout(left_);
  left_layout->setContentsMargins(0, 0, 0, 0);
  left_layout->setSpacing(grid_->spacing());
  // The settings at the bottom, Your tags all the height above them, scrolling past it.
  left_layout->addWidget(mine_, /*stretch=*/1);
  left_layout->addWidget(settings_card_);

  steam_ = new SettingsCard("From Steam", overview_);
  steam_search_ = new QLineEdit(steam_);
  steam_search_->setPlaceholderText("Find a tag");
  steam_search_->setClearButtonEnabled(true);
  steam_search_->setFixedWidth(180);
  connect(steam_search_, &QLineEdit::textChanged, this, &TagsPage::FilterSteam);
  steam_->Header()->addWidget(steam_search_);
  steam_->SetScrollable();
  steam_note_ = NoteRow(steam_);
  steam_->AddRow(steam_note_);
  steam_columns_ = new RowColumns(steam_);
  steam_->AddRow(steam_columns_);
  steam_rare_row_ = new SettingRow(QString(), {}, steam_);
  steam_rare_ = new QPushButton(steam_rare_row_);
  steam_rare_->setObjectName("text_button");
  connect(steam_rare_, &QPushButton::clicked, this, [this] {
    show_rare_steam_ = !show_rare_steam_;
    FilterSteam();
  });
  steam_rare_row_->Label()->hide();
  steam_rare_row_->SetLeading(steam_rare_);
  steam_->AddRow(steam_rare_row_);
  Arrange();

  stack_->addWidget(overview_);
  picker_ = new TagPicker(artwork_, stack_);
  stack_->addWidget(picker_);
  connect(picker_, &TagPicker::Closed, this, [this] {
    stack_->setCurrentWidget(overview_);
    emit PickerToggled();
    Refresh();
  });
  connect(picker_, &TagPicker::Applied, this, &TagsPage::GamesChanged);
  connect(picker_, &TagPicker::ZoomRequested, this, &TagsPage::ZoomRequested);

  bar_ = new ChangeBar(overview_);
  connect(bar_, &ChangeBar::DiscardClicked, this, [this] {
    ++asked_;
    bar_->SetText(QString());
    Sync();  // the switch or row that asked goes back
  });
  connect(bar_, &ChangeBar::SaveClicked, this, [this] {
    bar_->SetText(QString());
    if (auto apply = std::exchange(bar_apply_, nullptr)) apply();
  });

  refresh_timer_ = new QTimer(this);
  refresh_timer_->setSingleShot(true);
  refresh_timer_->setInterval(300);
  connect(refresh_timer_, &QTimer::timeout, this, &TagsPage::Refresh);
  connect(EventHub::Instance(), &EventHub::Received, this, [this](const std::string& type, const std::string&, bool live) {
    if (live && ChangesTags(type)) RefreshSoon();
  });
  Refresh();
}

void TagsPage::SetGames(const std::vector<GameSummary>& games) { games_ = games; }

void TagsPage::SetTileWidth(int width) {
  picker_->SetTileWidth(width);
}

TileRow TagsPage::Row() const {
  return PickerOpen() ? picker_->Row() : TileRow{};
}

bool TagsPage::PickerOpen() const {
  return stack_->currentWidget() == picker_;
}

void TagsPage::ToggleHidden() {
  if (PickerOpen()) picker_->ToggleHidden();
}

void TagsPage::Leave() {
  if (end_rename_) end_rename_();
  if (PickerOpen()) emit picker_->Closed();
  if (!bar_->isHidden()) emit bar_->DiscardClicked();
}

void TagsPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  Arrange();
}

void TagsPage::Arrange() {
  // Below this the Steam list is better under the other cards than squeezed beside them.
  constexpr int kTwoColumnWidth = 1000;
  const bool wide = width() >= kTwoColumnWidth;
  grid_->removeWidget(left_);
  grid_->removeWidget(steam_);
  for (int i = 0; i < 2; ++i) {
    grid_->setRowStretch(i, 0);
    grid_->setColumnStretch(i, 0);
  }
  grid_->addWidget(left_, 0, 0);
  if (wide) {
    grid_->addWidget(steam_, 0, 1);
    grid_->setColumnStretch(1, 1);
    grid_->setRowStretch(0, 1);
  } else {
    grid_->addWidget(steam_, 1, 0);
    grid_->setColumnStretch(0, 1);
    grid_->setRowStretch(1, 1);
  }
  FitLeft();
}

void TagsPage::FitLeft() {
  // A little wider than the widest row needs, so the switches keep some room from the names.
  constexpr int kSpare = 48;
  const bool wide = grid_->itemAtPosition(0, 1) != nullptr;
  const int needed = std::max(mine_->sizeHint().width(), settings_card_->sizeHint().width()) + kSpare;
  left_->setMinimumWidth(wide ? needed : 0);
  left_->setMaximumWidth(wide ? needed : QWIDGETSIZE_MAX);
  // Never so short that only a few rows show, as when the page stacks under 1000 px.
  const QScrollArea* scroll = mine_->Scroll();
  const int rows_height = mine_->sizeHint().height() - scroll->sizeHint().height() + scroll->widget()->sizeHint().height();
  mine_->setMinimumHeight(std::min(rows_height, 240));
}

void TagsPage::RefreshSoon() {
  // Not while asking or picking: rows changed under the question would lose what it's about.
  if (stack_->currentWidget() == overview_ && bar_->isHidden()) refresh_timer_->start();
}

void TagsPage::Refresh() {
  api::GetTagsAsync(this, [this](TagsResult result) {
    if (!result.ok) return Failed("Could not list the tags.", result.error);
    tags_ = std::move(result);
    tags_loaded_ = true;
    Sync();
    // Games fetched before Steam tags were get them now, once a session (the page is kept).
    if (tags_.steam_missing > 0 && settings_.steam && !fetching_steam_) {
      fetching_steam_ = true;
      SyncSteam();
      api::FetchSteamTagsAsync(this, [this](SteamTagsFetchResult fetched) {
        if (!fetched.ok) {
          fetching_steam_ = false;  // asked again on the next refresh
          return Failed("Could not get tags from Steam.", fetched.error);
        }
        Refresh();
      });
    }
  });
  api::GetConfigAsync(this, [this](ConfigResult result) {
    if (!result.ok) return Failed("Could not read the tag settings.", result.error);
    const auto value = [&](const char* key) {
      const auto found = result.values.find(key);
      return found != result.values.end() ? found->second : std::string();
    };
    settings_.roots = mapping::ParseListText(value("library_roots"));
    settings_.sorted_roots = mapping::ParseListText(value("tags.sorted_roots"));
    settings_.folders = mapping::ParseListText(value("tags.folders"));
    settings_.steam = value("tags.steam") != "false";
    settings_.steam_by_name = value("metadata.steam_by_name") != "false";
    settings_.tag_by_root = value("scan.tag_by_root") != "false";
    settings_loaded_ = true;
    Sync();
  });
}

void TagsPage::Sync() {
  if (!tags_loaded_ || !settings_loaded_) return;
  SyncMine();
  SyncSteam();
  SyncSettings();
}

const TagSummary* TagsPage::Mine(const std::string& name) const {
  const auto found =
      std::ranges::find_if(tags_.tags, [&](const TagSummary& t) { return t.name == name; });
  return found != tags_.tags.end() ? &*found : nullptr;
}

TagsPage::MineRow& TagsPage::MineRowFor(const std::string& name) {
  if (auto found = mine_rows_.find(name); found != mine_rows_.end()) return found->second;
  MineRow& entry = mine_rows_[name];
  const QString label = QString::fromStdString(name);
  entry.row = new SettingRow(label, {}, mine_);
  entry.row->SetGripShown(false);
  entry.row->Grip()->setToolTip(
      "A game with several folder tags goes in the one listed first. Drag to reorder, or press "
      "Alt+Up or "
      "Alt+Down");
  // In every row, shown or not, so the names line up.
  entry.icon = new QLabel(entry.row);
  icons::Follow(entry.icon, icons::Glyph::Folder, 14, &theme::Tokens::accent);
  entry.icon->setFixedSize(16, 16);
  entry.icon->setToolTip("A folder tag");
  KeepSpaceWhenHidden(entry.icon);
  entry.row->SetLeading(entry.icon);
  entry.count = CountLabel(entry.row);
  entry.row->AddControl(entry.count);
  // Centered in a column as wide as its heading.
  auto* folder_column = new QWidget(entry.row);
  folder_column->setFixedWidth(folder_width_);
  auto* folder_layout = new QHBoxLayout(folder_column);
  folder_layout->setContentsMargins(0, 0, 0, 0);
  entry.folder = new Switch(folder_column);
  entry.folder->setToolTip("A folder for it in each library folder sorted by tag");
  entry.folder->setAccessibleName(QString("%1 is a folder").arg(label));
  connect(entry.folder, &Switch::clicked, this, [this, name](bool on) { SetFolder(name, on); });
  folder_layout->addWidget(entry.folder, 0, Qt::AlignCenter);
  entry.row->AddControl(folder_column);
  auto* more = MoreButton(entry.row);
  more->setToolTip(QString("More for %1").arg(label));
  connect(more, &QToolButton::clicked, this, [this, name, more] { ShowMenu(name, more); });
  entry.row->AddControl(more);
  mine_->AddRow(entry.row);
  return entry;
}

void TagsPage::SyncMine() {
  // Folder tags first, in the order that picks a game's folder, then the rest, most games first.
  std::vector<const TagSummary*> order;
  for (const std::string& folder : settings_.folders) {
    const auto found = std::ranges::find_if(
        tags_.tags, [&](const TagSummary& t) { return SameTag(t.name, folder); });
    if (found != tags_.tags.end() && !std::ranges::contains(order, &*found))
      order.push_back(&*found);
  }
  for (const TagSummary& tag : tags_.tags) {
    if (!std::ranges::contains(order, &tag)) order.push_back(&tag);
  }

  for (auto it = mine_rows_.begin(); it != mine_rows_.end();) {
    if (Mine(it->first) == nullptr) {
      mine_->RemoveRow(it->second.row);
      it = mine_rows_.erase(it);
    } else {
      ++it;
    }
  }
  mine_note_->setVisible(order.empty());
  mine_->MoveRow(mine_note_, 0);
  for (std::size_t i = 0; i < order.size(); ++i) {
    const TagSummary& tag = *order[i];
    MineRow& entry = MineRowFor(tag.name);
    const bool folder = std::ranges::any_of(
        settings_.folders, [&](const std::string& f) { return SameTag(f, tag.name); });
    entry.row->SetGripShown(folder);
    entry.icon->setVisible(folder);
    entry.count->setText(QString::number(tag.ids.size()));
    entry.count->setToolTip(Games(tag.ids.size()));
    const QSignalBlocker block(entry.folder);
    entry.folder->setChecked(folder);
    mine_->MoveRow(entry.row, static_cast<int>(i) + 1);
  }
  // Once the layout has shown the new rows, which it does on the next pass of the event loop.
  QTimer::singleShot(0, this, &TagsPage::FitLeft);
}

TagsPage::SteamRow& TagsPage::SteamRowFor(const std::string& name) {
  if (auto found = steam_rows_.find(name); found != steam_rows_.end()) return found->second;
  SteamRow& entry = steam_rows_[name];
  const QString label = QString::fromStdString(name);
  // A SettingRow's shape, but a long name shrinks with an ellipsis to fit its column.
  entry.row = new SteamTagRow(steam_columns_);
  auto* line = new QHBoxLayout(entry.row);
  line->setContentsMargins(24, 8, 14, 8);
  line->setSpacing(8);
  auto* name_label = new ElidedLabel(label, entry.row);
  name_label->setMinimumHeight(30);
  line->addWidget(name_label, /*stretch=*/1);
  entry.count = new CountPill(entry.row);
  line->addWidget(entry.count);
  auto* add = new PlusButton(entry.row);
  add->setToolTip(QString("Choose the games to tag %1").arg(label));
  add->setAccessibleName(QString("Add %1").arg(label));
  connect(add, &QAbstractButton::clicked, this,
          [this, label] { OpenPicker(label, /*existing=*/false); });
  line->addWidget(add);
  steam_columns_->Place(entry.row, static_cast<int>(steam_rows_.size()));
  entry.row->show();
  return entry;
}

void TagsPage::SyncSteam() {
  const auto listed = [this](const std::string& name) {
    return std::ranges::any_of(tags_.steam,
                               [&](const SteamTagSuggestion& t) { return t.name == name; });
  };
  for (auto it = steam_rows_.begin(); it != steam_rows_.end();) {
    if (!settings_.steam || !listed(it->first)) {
      steam_columns_->Remove(it->second.row);
      it = steam_rows_.erase(it);
    } else {
      ++it;
    }
  }
  if (settings_.steam) {
    std::size_t most = 1;
    for (const SteamTagSuggestion& tag : tags_.steam) most = std::max(most, tag.ids.size());
    for (std::size_t i = 0; i < tags_.steam.size(); ++i) {
      const SteamTagSuggestion& tag = tags_.steam[i];
      SteamRow& entry = SteamRowFor(tag.name);
      entry.games = tag.ids.size();
      entry.count->Set(tag.ids.size(), most);
      steam_columns_->Place(entry.row, static_cast<int>(i));
    }
  }
  FilterSteam();
}

void TagsPage::FilterSteam() {
  const QString search = steam_search_->text().trimmed();
  int shown = 0;
  int rare = 0;
  for (auto& [name, entry] : steam_rows_) {
    const bool is_rare = entry.games < 2;
    rare += is_rare;
    const bool show = search.isEmpty()
                          ? !is_rare || show_rare_steam_
                          : QString::fromStdString(name).contains(search, Qt::CaseInsensitive);
    entry.row->setVisible(show);
    shown += show;
  }
  // Tags on one game are most of Steam's and say little about a library; search still finds them.
  steam_rare_row_->setVisible(search.isEmpty() && rare > 0);
  steam_rare_->setText(show_rare_steam_ ? QString("Hide the %1 on one game each").arg(rare)
                                        : QString("Show %1 more on one game each").arg(rare));
  QString note;
  if (!settings_.steam) {
    note = "Getting tags from Steam is off (Tag settings).";
  } else if (fetching_steam_ && tags_.steam_missing > 0) {
    note = "Getting tags from Steam…";
  } else if (shown == 0 && rare == 0) {
    note = search.isEmpty() ? QString("No Steam tags on your games yet.")
                            : QString("No Steam tag matches.");
  } else if (shown == 0 && !search.isEmpty()) {
    note = "No Steam tag matches.";
  }
  steam_note_->Label()->setText(note);
  steam_note_->setVisible(!note.isEmpty());
  steam_columns_->Relayout();
}

void TagsPage::SyncSettings() {
  // Rebuilt only when the library folders change; otherwise the switches follow in place.
  if (settings_roots_ != settings_.roots || setting_switches_.empty()) {
    settings_roots_ = settings_.roots;
    settings_card_->ClearRows();
    setting_switches_.clear();
    const auto add = [this](const std::string& key, const QString& label, const QString& doc,
                            std::function<void(bool)> changed) {
      auto* row = new SettingRow(label, doc, settings_card_);
      auto* toggle = new Switch(row);
      connect(toggle, &Switch::clicked, this, std::move(changed));
      row->AddControl(toggle);
      settings_card_->AddRow(row);
      setting_switches_[key] = toggle;
    };
    for (const std::string& root : settings_.roots) {
      add("sort:" + root, QString("Sort %1 into folders by tag").arg(FolderName(root)),
          "Moves each game's folder into the folder of its folder tag, and hidden games into "
          ".hidden. Moving "
          "a game's folder by hand changes its tags to match.",
          [this, root](bool on) { SetSorted(root, on); });
    }
    add("tags.steam", "Get tags from Steam",
        "Fetch each game's Steam tags with its other store info, to suggest them here. They're "
        "never added to "
        "a game on their own.",
        [this](bool on) { PatchSetting("tags.steam", "a boolean", on ? "true" : "false"); });
    add("metadata.steam_by_name", "Match other games to Steam by name",
        "Also get Steam tags, reviews and a ProtonDB rating for a game from another source from "
        "the Steam game of its name. This sends the game's name to Steam's public search.",
        [this](bool on) {
          PatchSetting("metadata.steam_by_name", "a boolean", on ? "true" : "false");
        });
    add("scan.tag_by_root", "Tag new games by library folder",
        "Tag each new game a scan finds with the name of the library folder it was found in.",
        [this](bool on) { PatchSetting("scan.tag_by_root", "a boolean", on ? "true" : "false"); });
  }
  const auto set = [this](const std::string& key, bool on) {
    Switch* toggle = setting_switches_.at(key);
    const QSignalBlocker block(toggle);
    toggle->setChecked(on);
  };
  for (const std::string& root : settings_.roots) {
    set("sort:" + root, std::ranges::any_of(settings_.sorted_roots, [&](const std::string& r) {
          return SameFolder(r, root);
        }));
  }
  set("tags.steam", settings_.steam);
  set("metadata.steam_by_name", settings_.steam_by_name);
  set("scan.tag_by_root", settings_.tag_by_root);
  QTimer::singleShot(0, this, &TagsPage::FitLeft);
}

void TagsPage::OpenPicker(const QString& name, bool existing) {
  std::vector<TagPicker::Section> sections;
  bool folder = false;
  if (existing) {
    const TagSummary* tag = Mine(name.toStdString());
    if (tag == nullptr) return;
    folder = tag->folder;
    std::vector<std::string> steam_only;
    for (const std::string& id : tag->steam_ids) {
      if (!std::ranges::contains(tag->ids, id)) steam_only.push_back(id);
    }
    sections.push_back({QString("Tagged %1").arg(name), tag->ids, true});
    sections.push_back({QString("Steam also tags these %1").arg(name), steam_only, false});
  } else if (!name.isEmpty()) {
    const auto tag = std::ranges::find_if(tags_.steam, [&](const SteamTagSuggestion& t) {
      return t.name == name.toStdString();
    });
    if (tag != tags_.steam.end()) sections.push_back({QString("Tagged %1 on Steam").arg(name), tag->ids, true});
  }
  picker_->Open(name, existing, std::move(sections), games_, folder, tags_.tags);
  stack_->setCurrentWidget(picker_);
  emit PickerToggled();
}

void TagsPage::ShowMenu(const std::string& name, QWidget* anchor) {
  const TagSummary* tag = Mine(name);
  if (tag == nullptr) return;
  QMenu menu(this);
  const QString label = QString::fromStdString(name);
  menu.addAction("Choose games…", this, [this, label] { OpenPicker(label, /*existing=*/true); });
  // Once the menu has closed and handed focus back, or the edit would lose it at once and end.
  bool rename = false;
  menu.addAction("Rename…", this, [&rename] { rename = true; });
  menu.addSeparator();
  menu.addAction("Remove from every game…", this, [this, name, label, count = tag->ids.size()] {
    ++asked_;
    bar_apply_ = [this, name] {
      api::RemoveTagAsync(this, name, [this](PatchGamesResult result) {
        if (!result.ok) return Failed("Could not remove that tag.", result.error);
        emit GamesChanged(result.games);
        Refresh();
      });
    };
    bar_->SetText(QString("Remove %1 from %2?").arg(label, Games(count)), "Remove", "Cancel");
  });
  menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
  if (rename) StartRename(name);
}

void TagsPage::StartRename(const std::string& name) {
  // In place of its row's name: Enter renames, Esc or leaving it keeps the old name.
  const auto found = mine_rows_.find(name);
  if (found == mine_rows_.end()) return;
  // One rename at a time: a menu takes focus without ending the last one's edit.
  if (end_rename_) end_rename_();
  SettingRow* row = found->second.row;
  auto* edit = new QLineEdit(QString::fromStdString(name), row);
  // As wide as the room between the name and the count, so the row's controls stay in the card.
  const int room = found->second.count->x() - row->Label()->x() - 12;
  edit->setFixedWidth(std::clamp(room, 80, 260));
  row->Label()->hide();
  row->SetLeading(edit);
  // Ends the edit at once; what follows (asking, the request) never touches the row, which a
  // refresh may replace meanwhile, so this checks both are still there.
  const auto finish = [row = QPointer<SettingRow>(row), edit = QPointer<QLineEdit>(edit)] {
    if (edit == nullptr || edit->isHidden()) return;
    edit->hide();
    edit->deleteLater();
    if (row != nullptr) row->Label()->show();
  };
  end_rename_ = finish;
  edit->installEventFilter(new EscapeCancels(edit, finish));
  // After the menu that asked closes, which hands focus back to where it was.
  QTimer::singleShot(0, edit, [edit] {
    edit->setFocus();
    edit->selectAll();
  });
  connect(edit, &QLineEdit::returnPressed, this, [this, name, edit, finish] {
    const std::string to = edit->text().trimmed().toStdString();
    finish();
    if (!to.empty() && to != name) Rename(name, to);
  });
  connect(edit, &QLineEdit::editingFinished, this, [edit, finish] {
    if (!edit->hasFocus()) finish();
  });
}

void TagsPage::Rename(const std::string& from, const std::string& to) {
  // Into a tag the library already has (not just a new spelling of this one): a merge, for good.
  const auto existing = std::ranges::find_if(tags_.tags, [&](const TagSummary& t) {
    return SameTag(t.name, to) && !SameTag(t.name, from);
  });
  const bool merge = existing != tags_.tags.end();
  const auto renamed = [&](std::vector<std::string> list) {
    for (std::string& tag : list) {
      if (SameTag(tag, from)) tag = to;
    }
    std::vector<std::string> unique;
    for (const std::string& tag : list) {
      if (std::ranges::none_of(unique, [&](const std::string& u) { return SameTag(u, tag); })) unique.push_back(tag);
    }
    return unique;
  };
  std::optional<std::vector<std::string>> folders;
  if (std::ranges::any_of(settings_.folders, [&](const std::string& f) { return SameTag(f, from); }))
    folders = renamed(settings_.folders);
  // The games with it, as the rename leaves them, so the preview counts their folders' moves.
  std::map<std::string, std::vector<std::string>> after;
  if (const TagSummary* tag = Mine(from)) {
    for (const std::string& id : tag->ids) {
      const auto game = std::ranges::find(games_, id, &GameSummary::id);
      if (game != games_.end()) after[id] = renamed(game->tags);
    }
  }
  const QString from_label = QString::fromStdString(from);
  const QString to_label = merge ? QString::fromStdString(existing->name) : QString::fromStdString(to);
  AskThenApply(
      folders, std::nullopt,
      [merge, from_label, to_label](int moving) {
        const QString moves = moving > 0 ? QString(" %1.").arg(FoldersMove(moving)) : QString();
        return merge ? QString("Merge %1 into %2? This can't be undone.%3").arg(from_label, to_label, moves)
                     : QString("Rename %1 to %2?%3").arg(from_label, to_label, moves);
      },
      merge ? "Merge" : "Rename",
      [this, from, to] {
        api::RenameTagAsync(this, from, to, [this](PatchGamesResult result) {
          if (!result.ok) return Failed("Could not rename that tag.", result.error);
          emit GamesChanged(result.games);
          Refresh();
        });
      },
      after, merge);
}

void TagsPage::FoldersReordered() {
  std::vector<std::string> folders;
  for (QWidget* row : mine_->Rows()) {
    const auto entry =
        std::ranges::find_if(mine_rows_, [&](const auto& pair) { return pair.second.row == row; });
    if (entry == mine_rows_.end()) continue;
    // As the setting spells it.
    const auto spelled = std::ranges::find_if(
        settings_.folders, [&](const std::string& f) { return SameTag(f, entry->first); });
    if (spelled != settings_.folders.end()) folders.push_back(*spelled);
  }
  if (folders == settings_.folders) return;
  AskThenApply(
      folders, std::nullopt,
      [](int moving) { return QString("Use this folder order? %1.").arg(FoldersMove(moving)); },
      "Reorder",
      [this, folders] {
        PatchSetting("tags.folders", "an array of strings", mapping::ListText(folders));
      });
}

void TagsPage::AskThenApply(std::optional<std::vector<std::string>> folders,
                            std::optional<std::vector<std::string>> sorted_roots, std::function<QString(int)> question,
                            const QString& apply_label, std::function<void()> apply,
                            const std::map<std::string, std::vector<std::string>>& tags, bool always_ask) {
  const int asked = ++asked_;
  api::PreviewTagsAsync(this, std::move(folders), std::move(sorted_roots), tags,
                        [this, asked, question, apply_label, apply, always_ask](FolderTagsPreviewResult result) {
                          if (asked != asked_) return;
                          if (!result.ok) {
                            Sync();
                            return Failed("Could not check what would move.", result.error);
                          }
                          const int moving = static_cast<int>(result.moving.size());
                          // Nothing on disk changes, so nothing to ask.
                          if (moving == 0 && !always_ask) return apply();
                          bar_apply_ = apply;
                          bar_->SetText(question(moving), apply_label, "Cancel");
                        });
}

void TagsPage::SetFolder(const std::string& name, bool folder) {
  const TagSummary* tag = Mine(name);
  if (tag == nullptr) return;
  const QString label = QString::fromStdString(name);
  AskThenApply(
      With(settings_.folders, name, folder), std::nullopt,
      [label, folder](int moving) {
        return folder ? QString("Use %1 as a folder? %2.").arg(label, FoldersMove(moving))
                      : QString("Stop using %1 as a folder? %2.").arg(label, FoldersMove(moving));
      },
      folder ? "Use as folder" : "Stop using as folder",
      [this, name, ids = tag->ids, folder] {
        api::SetTagAsync(this, name, ids, folder, [this](PatchGamesResult result) {
          if (!result.ok) {
            Sync();
            return Failed("Could not change that folder tag.", result.error);
          }
          emit GamesChanged(result.games);
          Refresh();
        });
      });
}

void TagsPage::SetSorted(const std::string& root, bool sorted) {
  std::vector<std::string> roots = settings_.sorted_roots;
  std::erase_if(roots, [&](const std::string& r) { return SameFolder(r, root); });
  if (sorted) roots.push_back(root);
  const QString name = FolderName(root);
  AskThenApply(
      std::nullopt, roots,
      [name, sorted](int moving) {
        return sorted ? QString("Sort %1 into folders by tag? %2, hidden ones into .hidden.").arg(name, FoldersMove(moving))
                      : QString("Stop sorting %1 by tag? %2 back.").arg(name, FoldersMove(moving));
      },
      sorted ? "Sort by tag" : "Stop sorting",
      [this, roots] { PatchSetting("tags.sorted_roots", "an array of strings", mapping::ListText(roots)); });
}

void TagsPage::PatchSetting(const std::string& key, const std::string& type, const std::string& value) {
  api::PatchConfigAsync(this, {{key, type, value}}, [this](PatchConfigResult result) {
    if (!result.ok) {
      Sync();
      return Failed("Could not save that setting.", result.error);
    }
    fetching_steam_ = false;  // Steam turned on, or matching widened: what's missing is fetched
    Refresh();
  });
}

void TagsPage::Failed(const QString& what, const ApiError& error) { notify::FailedRequest(window(), what, error); }

}  // namespace mira_gui
