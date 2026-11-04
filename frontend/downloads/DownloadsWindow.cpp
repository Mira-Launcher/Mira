#include "DownloadsWindow.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <format>
#include <utility>

#include "../activity/DownloadTracker.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Downloads.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ListEdit.h"
#include "../widgets/PopupDialog.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/Scrolling.h"
#include "../window/FramelessRoot.h"
#include "LinkDownloads.h"

namespace mira_gui {
namespace {

using State = DownloadTracker::State;

void ToggleMaximized(QWidget* window) {
  window->isMaximized() ? window->showNormal() : window->showMaximized();
}

// The window's own bar: dragging it moves the window.
class Bar : public QWidget {
 public:
  explicit Bar(QWidget* parent) : QWidget(parent) {
    setObjectName("top_bar");
    setAttribute(Qt::WA_StyledBackground);
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && window()->windowHandle() != nullptr) {
      window()->windowHandle()->startSystemMove();
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    ToggleMaximized(window());
    event->accept();
  }
};

QString Detail(const DownloadInfo& download) {
  const DownloadTracker::Entry entry = DownloadTracker::FromLink(download);
  if (download.state == "failed") return error_help::Describe(download.error);
  if (download.state == "done") return "Added to your Mira folder";
  if (!entry.message.isEmpty()) return entry.message;
  const QString progress = DownloadTracker::ProgressText(entry);
  if (download.state == "paused")
    return progress.isEmpty() ? QString("Paused") : "Paused  ·  " + progress;
  return progress.isEmpty() ? QString("Downloading…") : progress;
}

double Fraction(const DownloadInfo& download) {
  return DownloadTracker::FromLink(download).progress;
}

bool HasRail(const DownloadInfo& download) {
  return download.state == "downloading" || download.state == "paused" ||
         download.state == "resolving" || download.state == "extracting";
}

QString FileProgress(const DownloadFile& file) {
  if (file.state == "complete") return "Done";
  if (file.total <= 0) return QString();
  return QString("%1%").arg(file.done * 100 / file.total);
}

QPushButton* TextButton(QWidget* parent, const QString& text) {
  auto* button = new QPushButton(text, parent);
  button->setObjectName("text_button");
  return button;
}

}  // namespace

DownloadsWindow::DownloadsWindow(LinkDownloads* downloads, DownloadTracker* tracker,
                                 QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint),
      downloads_(downloads),
      tracker_(tracker) {
  setWindowTitle("Downloads");
  resize(820, 560);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* frame = new FramelessRoot(this);
  frame->setObjectName("downloads_frame");
  frame->setAttribute(Qt::WA_StyledBackground);
  outer->addWidget(frame);
  auto* root = new QVBoxLayout(frame);
  root->setContentsMargins(kResizeMargin, kResizeMargin, kResizeMargin, kResizeMargin);
  root->setSpacing(0);

  auto* bar = new Bar(frame);
  bar->setCursor(Qt::ArrowCursor);
  auto* bar_layout = new QHBoxLayout(bar);
  bar_layout->setContentsMargins(12, 4, 6, 4);
  bar_layout->setSpacing(10);
  auto* badge = new QLabel("M", bar);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  bar_layout->addWidget(badge);
  auto* title = new QLabel("Downloads", bar);
  title->setObjectName("downloads_title");
  bar_layout->addWidget(title);
  bar_layout->addStretch(1);
  const auto window_button = [bar, bar_layout](const QString& tooltip) {
    auto* button = new QToolButton(bar);
    button->setAutoRaise(true);
    button->setToolTip(tooltip);
    bar_layout->addWidget(button);
    return button;
  };
  minimize_ = window_button("Minimize");
  connect(minimize_, &QToolButton::clicked, this, &QWidget::showMinimized);
  maximize_ = window_button("Maximize");
  connect(maximize_, &QToolButton::clicked, this, [this] { ToggleMaximized(this); });
  close_ = window_button("Close");
  close_->setObjectName("close_button");
  connect(close_, &QToolButton::clicked, this, &QWidget::close);
  root->addWidget(bar);
  PaintChrome();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          &DownloadsWindow::PaintChrome);

  auto* body = new QWidget(frame);
  body->setCursor(Qt::ArrowCursor);
  auto* body_layout = new QVBoxLayout(body);
  body_layout->setContentsMargins(0, 0, 0, 0);
  body_layout->setSpacing(0);
  root->addWidget(body, 1);

  auto* tools = new QHBoxLayout();
  tools->setContentsMargins(16, 10, 16, 10);
  tools->setSpacing(10);
  speed_ = new QLabel(body);
  speed_->setStyleSheet("font-weight: 600;");
  tools->addWidget(speed_);
  tools->addStretch(1);
  tools->addWidget(MakeLabel(body, "Limit", "muted", false));
  limit_ = new QSpinBox(body);
  limit_->setRange(0, 100000);
  limit_->setSingleStep(5);
  limit_->setSuffix(" MB/s");
  limit_->setSpecialValueText("Unlimited");
  limit_->setKeyboardTracking(false);
  tools->addWidget(limit_);
  tools->addWidget(MakeLabel(body, "At once", "muted", false));
  parallel_ = new QSpinBox(body);
  parallel_->setRange(1, 10);
  parallel_->setKeyboardTracking(false);
  tools->addWidget(parallel_);
  pause_all_ = new QPushButton("Pause all", body);
  connect(pause_all_, &QPushButton::clicked, this, [this] {
    const auto& list = downloads_->Downloads();
    const bool running = std::ranges::any_of(
        list, [](const DownloadInfo& download) { return download.state == "downloading"; });
    for (const DownloadInfo& download : list) {
      if (running && download.state == "downloading") downloads_->Act(download.id, "pause");
      if (!running && download.state == "paused") downloads_->Act(download.id, "resume");
    }
  });
  tools->addWidget(pause_all_);
  auto* passwords = new QPushButton("Passwords", body);
  passwords->setToolTip("The archive passwords Mira tries");
  connect(passwords, &QPushButton::clicked, this, &DownloadsWindow::EditPasswords);
  tools->addWidget(passwords);
  body_layout->addLayout(tools);
  connect(limit_, &QSpinBox::valueChanged, this,
          [this](int value) { Save("downloads.speed_limit_mb", value); });
  connect(parallel_, &QSpinBox::valueChanged, this,
          [this](int value) { Save("downloads.parallel", value); });

  auto* scroll = new QScrollArea(body);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll, this);
  auto* list = new QWidget(scroll);
  rows_ = new QVBoxLayout(list);
  rows_->setContentsMargins(0, 0, 0, 12);
  rows_->setSpacing(0);
  scroll->setWidget(list);
  body_layout->addWidget(scroll, 1);

  connect(downloads_, &LinkDownloads::Changed, this, &DownloadsWindow::Refresh);
  connect(downloads_, &LinkDownloads::Removed, this, &DownloadsWindow::Refresh);
  connect(tracker_, &DownloadTracker::Changed, this, &DownloadsWindow::Refresh);
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string&, bool live) {
            if (live && type == "config.changed" && isVisible()) LoadSettings();
          });
}

void DownloadsWindow::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) PaintChrome();
}

void DownloadsWindow::PaintChrome() {
  minimize_->setIcon(icons::For(icons::Glyph::Minimize));
  maximize_->setIcon(icons::For(isMaximized() ? icons::Glyph::Restore : icons::Glyph::Maximize));
  close_->setIcon(icons::For(icons::Glyph::Close));
}

void DownloadsWindow::Open() {
  LoadSettings();
  downloads_->SeenFailures();
  Rebuild();
  show();
  setWindowState(windowState() & ~Qt::WindowMinimized);
  raise();
  activateWindow();
}

void DownloadsWindow::LoadSettings() {
  api::GetConfigAsync(this, [this](ConfigResult config) {
    if (!config.ok) return;
    const auto number = [&config](const char* key, int fallback) {
      const auto found = config.values.find(key);
      return found == config.values.end() ? fallback : std::atoi(found->second.c_str());
    };
    const QSignalBlocker quiet_limit(limit_);
    const QSignalBlocker quiet_parallel(parallel_);
    limit_->setValue(number("downloads.speed_limit_mb", 0));
    parallel_->setValue(number("downloads.parallel", 2));
  });
}

void DownloadsWindow::EditPasswords() {
  api::GetDownloadPasswordsAsync(this, [this](DownloadPasswordsResult result) {
    if (!result.ok)
      return notify::FailedRequest(this, "Could not read the saved passwords.", result.error);
    PopupDialog dialog(this, notify::Level::Info, "Saved passwords");
    dialog.SetMessage("Tried on every encrypted archive, most recently working first.");
    dialog.SetWidth(460);
    auto* list = new ListEdit(ListEdit::Kind::Text, "Add password", &dialog);
    QStringList items;
    for (const std::string& password : result.passwords) items << QString::fromStdString(password);
    list->SetItems(items);
    dialog.AddWidget(list);
    dialog.AddButton("Cancel", /*accept=*/false);
    dialog.AddButton("Save", /*accept=*/true, /*default_button=*/true);
    if (dialog.exec() != QDialog::Accepted) return;
    std::vector<std::string> edited;
    for (const QString& item : list->Items()) edited.push_back(item.toStdString());
    api::SetDownloadPasswordsAsync(this, edited, [this](StoreActionResult saved) {
      if (!saved.ok) notify::FailedRequest(this, "Could not save the passwords.", saved.error);
    });
  });
}

void DownloadsWindow::Save(const char* key, int value) {
  ConfigEdit edit;
  edit.key = key;
  edit.type = "an integer";
  edit.value = std::to_string(value);
  api::PatchConfigAsync(this, {edit}, [](PatchConfigResult) {});
}

std::string DownloadsWindow::Shape() const {
  std::string shape;
  for (const DownloadInfo& download : downloads_->Downloads()) {
    shape += std::format("{}:{}:{}:{}:{};", download.id, download.state, download.error.code,
                         download.files.size(), expanded_.contains(download.id));
  }
  for (const DownloadTracker::Entry& entry : tracker_->Entries()) {
    if (entry.kind == DownloadTracker::Kind::Title && entry.state != State::Finished)
      shape += std::format("{}:{};", entry.key.toStdString(), static_cast<int>(entry.state));
  }
  return shape;
}

void DownloadsWindow::Refresh() {
  if (!isVisible()) return;
  if (Shape() == shape_) {
    UpdateRows();
  } else {
    Rebuild();
  }
}

void DownloadsWindow::UpdateHeader() {
  std::int64_t speed = 0;
  bool running = false;
  bool paused = false;
  for (const DownloadInfo& download : downloads_->Downloads()) {
    speed += download.speed;
    running = running || download.state == "downloading";
    paused = paused || download.state == "paused";
  }
  speed_->setText(running ? "↓ " + SizeText(speed) + "/s" : QString("Nothing downloading"));
  pause_all_->setText(running || !paused ? "Pause all" : "Resume all");
  pause_all_->setEnabled(running || paused);
}

void DownloadsWindow::UpdateRows() {
  UpdateHeader();
  for (const DownloadInfo& download : downloads_->Downloads()) {
    const auto found = parts_.find(download.id);
    if (found == parts_.end()) continue;
    const Parts& parts = found->second;
    parts.detail->setText(Detail(download));
    if (parts.rail != nullptr) parts.rail->SetProgress(Fraction(download));
    for (std::size_t i = 0; i < parts.files.size() && i < download.files.size(); ++i)
      parts.files[i]->setText(FileProgress(download.files[i]));
  }
  for (const DownloadTracker::Entry& entry : tracker_->Entries()) {
    const auto found = parts_.find(entry.key.toStdString());
    if (found == parts_.end()) continue;
    found->second.detail->setText(DownloadTracker::ProgressText(entry));
    if (found->second.rail != nullptr) found->second.rail->SetProgress(entry.progress);
  }
}

void DownloadsWindow::AddSection(const QString& heading) {
  auto* row = new QWidget();
  auto* layout = new QVBoxLayout(row);
  layout->setContentsMargins(16, 12, 16, 4);
  layout->addWidget(MakeGroupHeading(row, heading));
  rows_->addWidget(row);
}

void DownloadsWindow::Rebuild() {
  shape_ = Shape();
  parts_.clear();
  UpdateHeader();
  while (QLayoutItem* item = rows_->takeAt(0)) {
    // Deferred: a row's own button click may be what led here.
    if (item->widget() != nullptr) {
      item->widget()->hide();
      item->widget()->deleteLater();
    }
    delete item;
  }
  const auto& list = downloads_->Downloads();
  const auto section = [this, &list](const QString& heading,
                                     std::initializer_list<const char*> states) {
    bool first = true;
    for (const DownloadInfo& download : list) {
      if (std::ranges::none_of(states,
                               [&download](const char* state) { return download.state == state; }))
        continue;
      if (std::exchange(first, false)) AddSection(heading);
      rows_->addWidget(BuildRow(download));
    }
  };
  section("Needs you", {"password", "failed"});
  section("Active", {"resolving", "downloading", "paused", "extracting"});
  section("Queued", {"queued"});

  bool first_store = true;
  for (const DownloadTracker::Entry& entry : tracker_->Entries()) {
    if (entry.kind != DownloadTracker::Kind::Title || entry.state == State::Finished) continue;
    if (std::exchange(first_store, false)) AddSection("Stores");
    rows_->addWidget(BuildStoreRow(entry.key));
  }
  section("Finished", {"done"});
  if (rows_->count() == 0) {
    auto* none =
        MakeLabel(nullptr, "Nothing here yet. Add games has Download from links.", "subtle");
    none->setContentsMargins(16, 16, 16, 16);
    rows_->addWidget(none);
  }
  rows_->addStretch(1);
}

QWidget* DownloadsWindow::BuildStoreRow(const QString& key) {
  const DownloadTracker::Entry& entry = *tracker_->Find(key);
  auto* row = new QFrame();
  row->setObjectName("download_row");
  row->setProperty("first", false);
  auto* layout = new QVBoxLayout(row);
  layout->setContentsMargins(16, 8, 16, 8);
  layout->setSpacing(4);
  auto* title = new ElidedLabel(tracker_->NameFor(entry), row);
  title->setStyleSheet("font-weight: 600;");
  layout->addWidget(title);
  Parts parts;
  const QString detail = entry.state == State::Paused   ? QString("Paused")
                         : entry.state == State::Failed ? error_help::Describe(entry.error)
                                                        : DownloadTracker::ProgressText(entry);
  parts.detail = MakeLabel(row, detail, entry.state == State::Failed ? "error" : "muted");
  layout->addWidget(parts.detail);
  if (entry.state != State::Failed) {
    parts.rail = new ProgressRail(row);
    parts.rail->SetProgress(entry.progress);
    layout->addWidget(parts.rail);
  }
  parts_[key.toStdString()] = std::move(parts);
  return row;
}

QWidget* DownloadsWindow::BuildRow(const DownloadInfo& download) {
  const std::string id = download.id;
  auto* row = new QFrame();
  row->setObjectName("download_row");
  row->setProperty("first", false);
  auto* layout = new QVBoxLayout(row);
  layout->setContentsMargins(16, 8, 16, 8);
  layout->setSpacing(4);

  auto* line = new QHBoxLayout();
  line->setSpacing(8);
  auto* title = new ElidedLabel(QString::fromStdString(download.name), row);
  title->setStyleSheet("font-weight: 600;");
  title->setToolTip(title->text());
  line->addWidget(title, 1);
  if (!download.files.empty()) {
    const bool open = expanded_.contains(id);
    auto* files = TextButton(
        row, open ? QString("Hide files") : QString("Files (%1)").arg(download.files.size()));
    connect(files, &QPushButton::clicked, this, [this, id, open] {
      if (open) {
        expanded_.erase(id);
      } else {
        expanded_.insert(id);
      }
      Refresh();
    });
    line->addWidget(files);
  }
  const auto action = [&](const QString& text, const char* name) {
    auto* button = TextButton(row, text);
    connect(button, &QPushButton::clicked, this, [this, id, name] { downloads_->Act(id, name); });
    line->addWidget(button);
  };
  if (download.state == "queued") action("Move to front", "front");
  if (download.state == "downloading" || download.state == "queued") action("Pause", "pause");
  if (download.state == "paused") action("Resume", "resume");
  if (download.state == "failed") action("Retry", "retry");
  if (download.state != "extracting") {
    auto* remove = TextButton(row, download.state == "done" ? "Clear" : "Cancel");
    connect(remove, &QPushButton::clicked, this, [this, id] { downloads_->Remove(id); });
    line->addWidget(remove);
  }
  layout->addLayout(line);

  Parts parts;
  parts.detail = MakeLabel(row, Detail(download), download.state == "failed" ? "error" : "muted");
  layout->addWidget(parts.detail);
  if (HasRail(download)) {
    parts.rail = new ProgressRail(row);
    parts.rail->SetProgress(Fraction(download));
    layout->addWidget(parts.rail);
  }

  if (download.state == "password") {
    auto* entry = new QHBoxLayout();
    entry->setSpacing(8);
    auto* password = new QLineEdit(typed_[id], row);
    password->setPlaceholderText("Password");
    connect(password, &QLineEdit::textChanged, this,
            [this, id](const QString& text) { typed_[id] = text; });
    entry->addWidget(password, 1);
    auto* extract = new QPushButton("Extract", row);
    const auto submit = [this, id, password] {
      if (password->text().isEmpty()) return;
      const std::string text = password->text().toStdString();
      typed_.erase(id);
      downloads_->Unskip(id);
      downloads_->SubmitPassword(id, text);
    };
    connect(extract, &QPushButton::clicked, this, submit);
    connect(password, &QLineEdit::returnPressed, this, submit);
    entry->addWidget(extract);
    layout->addLayout(entry);
    if (download.error.code == "wrong_password")
      layout->addWidget(MakeLabel(row, "That password didn't open it.", "error"));
  }

  if (expanded_.contains(id)) {
    for (const DownloadFile& file : download.files) {
      auto* file_line = new QHBoxLayout();
      file_line->setContentsMargins(14, 0, 0, 0);
      file_line->setSpacing(10);
      file_line->addWidget(new ElidedLabel(QString::fromStdString(file.name), row), 1);
      auto* progress = MakeLabel(row, FileProgress(file), "muted", false);
      file_line->addWidget(progress);
      file_line->addWidget(MakeLabel(row, QString::fromStdString(file.source), "muted", false));
      parts.files.push_back(progress);
      layout->addLayout(file_line);
    }
  }
  parts_[id] = std::move(parts);
  return row;
}

}  // namespace mira_gui
