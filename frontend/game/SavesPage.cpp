#include "SavesPage.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

#include "../app/Notify.h"
#include "../client/JsonMapping.h"
#include "../client/api/Games.h"
#include "../library/GamePresentation.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/LaidOutButton.h"
#include "../widgets/Scrolling.h"
#include "SavesConflictCard.h"
#include "SavesText.h"

namespace mira_gui {

namespace {
constexpr int kColumnWidth = 340;  // the narrowest a column gets before the page goes to one

// A row inside a card, padded like its SettingRows.
QWidget* Row(QWidget* parent, QHBoxLayout** layout) {
  auto* row = new QWidget(parent);
  *layout = new QHBoxLayout(row);
  (*layout)->setContentsMargins(18, 6, 12, 6);
  (*layout)->setSpacing(10);
  return row;
}

QPushButton* TextButton(QWidget* parent, const QString& text, bool muted = false) {
  auto* button = new QPushButton(text, parent);
  button->setObjectName("text_button");
  if (muted) button->setProperty("tone", "muted");
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

// Two lines under each other: `top` as is, `bottom` muted.
QWidget* TwoLines(QWidget* parent, const QString& top, const QString& bottom, bool mono = false) {
  auto* box = new QWidget(parent);
  auto* lines = new QVBoxLayout(box);
  lines->setContentsMargins(0, 0, 0, 0);
  lines->setSpacing(2);
  QLabel* first = MakeLabel(box, top);
  if (mono) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(first->font().pointSizeF() * 0.92);
    first->setFont(font);
    first->setTextInteractionFlags(Qt::TextSelectableByMouse);
  }
  lines->addWidget(first);
  if (!bottom.isEmpty()) lines->addWidget(MakeLabel(box, bottom, "muted"));
  return box;
}

QString Files(int files, std::int64_t bytes) {
  return (files == 1 ? QString("1 file") : QString("%1 files").arg(files)) + " · " +
         SizeText(bytes);
}
}  // namespace

SavesPage::SavesPage(std::string id, QWidget* parent) : QWidget(parent), id_(std::move(id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(12, 12, 18, 4);
  header->setSpacing(12);
  auto* back = new QToolButton(this);
  back->setAutoRaise(true);
  back->setToolTip("Back");
  icons::Follow(back, icons::Glyph::ArrowLeft);
  connect(back, &QToolButton::clicked, this, &SavesPage::BackClicked);
  auto* title = new QLabel("Saves", this);
  title->setProperty("role", "heading");
  back_up_ = new QPushButton("Back up now", this);
  connect(back_up_, &QPushButton::clicked, this, &SavesPage::BackUp);
  header->addWidget(back);
  header->addWidget(title);
  header->addStretch(1);
  header->addWidget(back_up_);
  layout->addLayout(header);

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setStyleSheet(
      "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  scroll_->viewport()->setAutoFillBackground(false);
  SetUpScrolling(scroll_, this);
  auto* content = new QWidget();
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(18, 10, 18, 18);
  grid_ = new QGridLayout();
  grid_->setSpacing(12);
  content_layout->addLayout(grid_);
  content_layout->addStretch(1);
  scroll_->setWidget(content);
  layout->addWidget(scroll_, 1);

  backups_ = new SettingsCard("Backups", content);
  where_ = new SettingsCard("Where they are", content);
  cloud_ = new SettingsCard("Cloud saves", content);
  settings_ = new SettingsCard("Settings", content);
  cloud_->hide();
  // Rebuilt on a theme change: the newest backup's tint and the state dots are drawn colors.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    FillBackups();
    FillCloud();
  });
}

void SavesPage::Show(const SavesResult& saves) {
  saves_ = saves;
  doing_ = saves.doing;
  HoldFocus();
  back_up_->setVisible(saves_.status != "registry_only" && saves_.status != "no_entry");
  FillBackups();
  FillWhere();
  FillCloud();
  LoadSettings();
  SetBusy(busy_);
  Arrange();
}

void SavesPage::SetDoing(const std::string& doing) {
  if (doing == doing_) return;
  doing_ = doing;
  HoldFocus();
  FillCloud();
  SetBusy(busy_);
}

void SavesPage::HoldFocus() {
  // A focused button that's disabled or rebuilt hands focus to the next one, and the scroll area
  // jumps to show it.
  if (QWidget* focus = QApplication::focusWidget(); focus != nullptr && isAncestorOf(focus))
    scroll_->setFocus(Qt::OtherFocusReason);
}

void SavesPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  Arrange();
}

void SavesPage::Arrange() {
  // From the page's own width: the viewport's lags behind until the page has been shown.
  const int room = width() - 36 - scroll_->verticalScrollBar()->sizeHint().width();
  const int columns = room >= 2 * kColumnWidth + grid_->spacing() ? 2 : 1;
  if (columns == columns_) return;
  columns_ = columns;
  for (QWidget* card : {static_cast<QWidget*>(backups_), static_cast<QWidget*>(where_),
                        static_cast<QWidget*>(cloud_), static_cast<QWidget*>(settings_)})
    grid_->removeWidget(card);
  // Backups on the left, the rest stacked beside them; one column puts them under each other.
  if (columns == 2) {
    grid_->addWidget(backups_, 0, 0, 3, 1, Qt::AlignTop);
    grid_->addWidget(where_, 0, 1, Qt::AlignTop);
    grid_->addWidget(cloud_, 1, 1, Qt::AlignTop);
    grid_->addWidget(settings_, 2, 1, Qt::AlignTop);
    grid_->setColumnStretch(0, 1);
    grid_->setColumnStretch(1, 1);
    grid_->setRowStretch(2, 1);
  } else {
    grid_->addWidget(backups_, 0, 0);
    grid_->addWidget(where_, 1, 0);
    grid_->addWidget(cloud_, 2, 0);
    grid_->addWidget(settings_, 3, 0);
    grid_->setColumnStretch(0, 1);
    grid_->setColumnStretch(1, 0);
    grid_->setRowStretch(2, 0);
  }
}

void SavesPage::FillBackups() {
  backups_->ClearRows();
  QHBoxLayout* line = nullptr;
  const QDateTime now = QDateTime::currentDateTime();

  if (restored_) {
    QWidget* row = Row(backups_, &line);
    line->addWidget(MakeLabel(row, "Restored the backup from " +
                                       saves_text::When(restored_->taken_at, now, true) + "."),
                    1);
    if (!restored_->undo.empty()) {
      QPushButton* undo = TextButton(row, "Undo");
      undo->setEnabled(!Busy());
      connect(undo, &QPushButton::clicked, this, [this, undo_id = restored_->undo] {
        const auto found = std::ranges::find(saves_.snapshots, undo_id, &SaveSnapshot::id);
        if (found != saves_.snapshots.end()) Restore(*found);
      });
      line->addWidget(undo);
    }
    backups_->AddRow(row);
  }
  if (saves_.last_error) {
    QWidget* row = Row(backups_, &line);
    QWidget* text = TwoLines(row,
                             "The last backup after playing failed: " +
                                 QString::fromStdString(saves_.last_error->message),
                             QString::fromStdString(saves_.last_error->hint));
    if (auto* first = text->findChild<QLabel*>()) first->setProperty("role", "warning");
    line->addWidget(text, 1);
    backups_->AddRow(row);
  }
  if (saves_.snapshots.empty()) {
    QWidget* row = Row(backups_, &line);
    line->addWidget(MakeLabel(row, "No backups yet", "muted"), 1);
    backups_->AddRow(row);
    return;
  }
  const theme::Tokens& tokens = theme::Current();
  for (std::size_t index = 0; index < saves_.snapshots.size(); ++index) {
    const SaveSnapshot& snapshot = saves_.snapshots[index];
    QWidget* row = Row(backups_, &line);
    // The newest is what the game plays with now, unless something changed since.
    if (index == 0) {
      row->setAttribute(Qt::WA_StyledBackground);
      row->setStyleSheet(QString("QWidget { background: %1; border-radius: %2px; }")
                             .arg(tokens.surface_alt.name())
                             .arg(tokens.radius_control));
    }
    line->addWidget(
        TwoLines(row, saves_text::When(snapshot.taken_at, now),
                 saves_text::Reason(snapshot.reason) + " · " + SizeText(snapshot.bytes) +
                     (snapshot.pinned ? QString(" · Pinned") : QString())),
        1);
    auto* pin = new QToolButton(row);
    pin->setAutoRaise(true);
    pin->setCheckable(true);
    pin->setChecked(snapshot.pinned);
    pin->setCursor(Qt::PointingHandCursor);
    pin->setToolTip(snapshot.pinned ? "Unpin: let it go once newer backups take its place"
                                    : "Pin: keep it however many newer backups are taken");
    pin->setIcon(
        icons::For(icons::Glyph::Pin, snapshot.pinned ? tokens.accent : tokens.text_muted));
    pin->setEnabled(!Busy());
    connect(pin, &QToolButton::clicked, this, [this, snapshot] { Pin(snapshot); });
    QPushButton* restore = TextButton(row, "Restore");
    restore->setToolTip("Put these saves back. What's there now is backed up first.");
    restore->setEnabled(!Busy());
    connect(restore, &QPushButton::clicked, this, [this, snapshot] { Restore(snapshot); });
    QPushButton* remove = TextButton(row, "Delete", /*muted=*/true);
    remove->setEnabled(!Busy());
    connect(remove, &QPushButton::clicked, this, [this, snapshot] { Delete(snapshot); });
    line->addWidget(pin);
    line->addWidget(restore);
    line->addWidget(remove);
    backups_->AddRow(row);
  }
}

void SavesPage::FillWhere() {
  where_->ClearRows();
  QHBoxLayout* line = nullptr;
  const QString home = QDir::homePath();
  for (const SaveLocation& location : saves_.locations) {
    QWidget* row = Row(where_, &line);
    const QString problem = location.problem == "too_broad"
                                ? QString("Not backed up: it would take a whole folder")
                            : location.problem == "unknown_placeholder"
                                ? QString("Not backed up: this game doesn't have that folder")
                                : QString();
    QWidget* text = TwoLines(row, saves_text::Path(location.path, home),
                             problem.isEmpty() ? Files(location.files, location.bytes) : problem,
                             /*mono=*/true);
    text->setToolTip(QString::fromStdString(location.path));
    line->addWidget(text, 1);
    if (location.extra) {
      QPushButton* remove = TextButton(row, "Remove", /*muted=*/true);
      remove->setToolTip("Stop backing up this folder. Its files stay where they are.");
      remove->setEnabled(!Busy());
      connect(remove, &QPushButton::clicked, this,
              [this, pattern = location.pattern] { RemoveFolder(pattern); });
      line->addWidget(remove, 0, Qt::AlignTop);
    }
    where_->AddRow(row);
  }
  // Where to look for what the list doesn't know: the game's prefix or its folder.
  const std::string& look_in =
      !saves_.prefix_folder.empty() ? saves_.prefix_folder : saves_.install_folder;
  if (!look_in.empty()) {
    QWidget* row = Row(where_, &line);
    QPushButton* add = TextButton(row, "Add a folder…");
    add->setToolTip("Back up another folder of this game's saves too");
    add->setEnabled(!Busy());
    connect(add, &QPushButton::clicked, this, &SavesPage::AddFolder);
    QPushButton* open =
        TextButton(row, saves_.prefix_folder.empty() ? "Open game folder" : "Open prefix",
                   /*muted=*/true);
    open->setToolTip(QString::fromStdString(look_in));
    open->setProperty("idle_only", false);  // looking doesn't touch the saves
    connect(open, &QPushButton::clicked, this, [look_in] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(look_in)));
    });
    line->addWidget(add);
    line->addWidget(open);
    line->addStretch(1);
    where_->AddRow(row);
  }
  QStringList notes;
  if (const QString status = saves_text::StatusNote(saves_); !status.isEmpty()) notes << status;
  if (!saves_.entry.empty())
    notes << "From the save locations list: " + QString::fromStdString(saves_.entry);
  for (const QString& note : notes) {
    QWidget* row = Row(where_, &line);
    line->addWidget(MakeLabel(row, note, "muted"), 1);
    where_->AddRow(row);
  }
}

void SavesPage::FillCloud() {
  cloud_->ClearRows();
  cloud_->setVisible(saves_.cloud.has_value());
  if (!saves_.cloud) return;
  const CloudSaves& cloud = *saves_.cloud;
  const theme::Tokens& tokens = theme::Current();
  const QDateTime now = QDateTime::currentDateTime();
  QHBoxLayout* line = nullptr;

  QWidget* state_row = Row(cloud_, &line);
  // While mirad works on the saves (after play, a backup then the sync), the state it last found
  // is about to change, so the work is what the line says.
  const bool working = !doing_.empty();
  const bool conflict = !working && cloud.state == "conflict";
  const QColor dot = working                   ? tokens.info
                     : conflict                ? tokens.warning
                     : cloud.state == "synced" ? tokens.success
                                               : tokens.text_muted;
  auto* state = MakeLabel(state_row, StatusDot(dot) + (working ? SavesDoingLabel(doing_)
                                                               : saves_text::CloudState(cloud)));
  state->setTextFormat(Qt::RichText);
  QWidget* text = new QWidget(state_row);
  auto* lines = new QVBoxLayout(text);
  lines->setContentsMargins(0, 0, 0, 0);
  lines->setSpacing(2);
  lines->addWidget(state);
  lines->addWidget(MakeLabel(text,
                             cloud.synced_at > 0
                                 ? "Last synced " + saves_text::When(cloud.synced_at, now, true)
                                 : QString("Never synced"),
                             "muted"));
  line->addWidget(text, 1);
  const bool can_sync = cloud.state != "unsupported" && cloud.state != "off";
  if (!conflict && can_sync) {
    QPushButton* sync = TextButton(state_row, "Sync now");
    sync->setEnabled(!Busy());
    connect(sync, &QPushButton::clicked, this, [this] { Sync("sync"); });
    line->addWidget(sync, 0, Qt::AlignTop);
  }
  cloud_->AddRow(state_row);
  if (!conflict) return;

  // Each side with what it holds, as the launch's question shows them.
  QWidget* choices = Row(cloud_, &line);
  const auto choice = [&](const QString& title, const QString& detail, const QString& effect,
                          const std::string& action) {
    LaidOutButton* button = MakeSavesChoice(choices, title, detail, effect);
    button->setEnabled(!Busy());
    connect(button, &QPushButton::clicked, this, [this, action] { Sync(action); });
    line->addWidget(button, 1);
  };
  choice("Keep this computer's",
         cloud.local_at > 0 ? "Saved " + saves_text::When(cloud.local_at, now, true) + " · " +
                                  Files(cloud.files, cloud.bytes)
                            : QString(),
         "Uploads them over the cloud's", "upload");
  choice("Use the cloud's",
         cloud.cloud_at > 0 ? "Uploaded " + saves_text::When(cloud.cloud_at, now, true) : QString(),
         "This computer's are backed up first", "download");
  cloud_->AddRow(choices);
}

void SavesPage::LoadSettings() {
  const int request = ++settings_request_;
  api::GetGameConfigAsync(this, id_, [this, request](GameConfigResult result) {
    if (request != settings_request_) return;
    settings_->ClearRows();
    if (!result.ok) return;
    const auto value = [&](const std::string& key) {
      const auto found = std::ranges::find(result.entries, key, &GameConfigEntry::key);
      return found != result.entries.end() ? found->value_display : std::string();
    };
    struct Setting {
      std::string key;
      QString label;
      QString shown;
    };
    const auto on_off = [](const std::string& text) {
      return text == "true" ? QString("On") : QString("Off");
    };
    std::vector<Setting> rows = {
        {"saves.enabled", "Back up saves after playing", on_off(value("saves.enabled"))},
        {"saves.keep", "Backups kept", QString::fromStdString(value("saves.keep"))},
    };
    const std::vector<std::string> extra = mapping::ParseListText(value("saves.extra_paths"));
    rows.push_back({"saves.extra_paths", "More save locations",
                    extra.empty() ? QString("None") : QString::number(extra.size())});
    if (saves_.cloud) {
      rows.push_back(
          {"saves.cloud",
           QString("Sync with %1's cloud").arg(QString::fromStdString(saves_.cloud->store)),
           on_off(value("saves.cloud"))});
    }
    for (const Setting& setting : rows) {
      QHBoxLayout* line = nullptr;
      QWidget* row = Row(settings_, &line);
      line->addWidget(MakeLabel(row, setting.label), 1);
      QPushButton* shown = TextButton(row, setting.shown);
      shown->setToolTip("Change it in this game's Advanced settings");
      connect(shown, &QPushButton::clicked, this,
              [this, key = setting.key] { emit SettingClicked(key); });
      line->addWidget(shown);
      settings_->AddRow(row);
    }
  });
}

void SavesPage::SetBusy(bool busy) {
  busy_ = busy;
  if (Busy()) HoldFocus();
  back_up_->setEnabled(!Busy());
  back_up_->setText(!doing_.empty() ? SavesDoingLabel(doing_)
                    : busy          ? QString("Working…")
                                    : QString("Back up now"));
  for (SettingsCard* card : {backups_, where_, cloud_})
    for (QAbstractButton* button : card->findChildren<QAbstractButton*>())
      if (!button->property("idle_only").isValid() || button->property("idle_only").toBool())
        button->setEnabled(!Busy());
}

void SavesPage::BackUp() {
  restored_.reset();
  SetBusy(true);
  api::BackUpSavesAsync(this, id_, [this](SavesJobResult result) {
    SetBusy(false);
    if (!result.ok) {
      notify::FailedRequest(this, "Could not back up the saves.", result.error);
      return;
    }
    // Nothing changed since the newest: the list already shows it, so say so.
    if (!result.taken) notify::Notice(this, "Nothing changed since the last backup.");
  });
}

void SavesPage::Restore(const SaveSnapshot& snapshot) {
  SetBusy(true);
  api::RestoreSavesAsync(this, id_, snapshot.id, [this, snapshot](SavesJobResult result) {
    SetBusy(false);
    if (!result.ok) {
      notify::FailedRequest(this, "Could not restore the saves.", result.error);
      return;
    }
    restored_ = Restored{snapshot.taken_at, result.undo};
    FillBackups();
  });
}

void SavesPage::Delete(const SaveSnapshot& snapshot) {
  if (!notify::Confirm(this, "Delete this backup?",
                       "The saves from " +
                           saves_text::When(snapshot.taken_at, QDateTime::currentDateTime(), true) +
                           " can't be restored once it's gone.",
                       "Delete", /*destructive=*/true))
    return;
  if (restored_ && restored_->undo == snapshot.id) restored_.reset();
  api::DeleteSaveBackupAsync(this, id_, snapshot.id, [this](GameActionResult result) {
    if (!result.ok) notify::FailedRequest(this, "Could not delete the backup.", result.error);
  });
}

void SavesPage::Sync(const std::string& action) {
  restored_.reset();
  SetBusy(true);
  api::SyncCloudSavesAsync(this, id_, action, [this](SavesJobResult result) {
    SetBusy(false);
    // A conflict is what the cloud card shows next, with its two choices.
    if (!result.ok && result.error.code != "saves_conflict")
      notify::FailedRequest(this, "Could not sync the saves.", result.error);
  });
}

void SavesPage::Pin(const SaveSnapshot& snapshot) {
  api::PinSaveBackupAsync(
      this, id_, snapshot.id, !snapshot.pinned, [this](GameActionResult result) {
        if (!result.ok)
          notify::FailedRequest(this, "Could not change the backup's pin.", result.error);
      });
}

void SavesPage::AddFolder() {
  const std::string& start =
      !saves_.prefix_folder.empty() ? saves_.prefix_folder : saves_.install_folder;
  const QString folder = QFileDialog::getExistingDirectory(this, "Folder with this game's saves",
                                                           QString::fromStdString(start));
  if (folder.isEmpty()) return;
  api::ChangeSaveFolderAsync(this, id_, /*add=*/true, QDir::cleanPath(folder).toStdString(),
                             [this](GameActionResult result) {
                               if (!result.ok)
                                 notify::FailedRequest(this, "Could not add the folder.",
                                                       result.error);
                             });
}

void SavesPage::RemoveFolder(const std::string& pattern) {
  api::ChangeSaveFolderAsync(this, id_, /*add=*/false, pattern, [this](GameActionResult result) {
    if (!result.ok) notify::FailedRequest(this, "Could not remove the folder.", result.error);
  });
}

}  // namespace mira_gui
