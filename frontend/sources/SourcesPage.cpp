#include "SourcesPage.h"

#include <QButtonGroup>
#include <QEvent>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Library.h"
#include "../client/api/Emulators.h"
#include "../client/api/Stores.h"
#include "../system/PackageInstall.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ModalOverlay.h"
#include "../widgets/PopupDialog.h"
#include "../widgets/Scrolling.h"
#include "SourceCatalog.h"
#include "SourceSetupCard.h"
#include "Sources.h"

namespace mira_gui {

namespace {

const char* kAddedHint = "Click a source to open it. Turning off its games keeps them out of the library; nothing is deleted.";
const char* kCatalogHint = "Pick a source to set up. Search by name, kind or what it does.";

}  // namespace

SourcesPage::SourcesPage(QWidget* parent) : QFrame(parent) {
  setObjectName("sources_card");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(24, 18, 14, 14);
  header->setSpacing(10);
  auto* titles = new QVBoxLayout();
  titles->setSpacing(2);
  auto* title = new QLabel("Sources", this);
  title->setObjectName("page_title");
  hint_ = MakeLabel(this, kAddedHint, "subtle");
  titles->addWidget(title);
  titles->addWidget(hint_);
  header->addLayout(titles, /*stretch=*/1);

  search_ = new QLineEdit(this);
  search_->setPlaceholderText("Search sources");
  search_->setClearButtonEnabled(true);
  search_->setFixedWidth(220);
  search_->addAction(icons::For(icons::Glyph::Search, theme::Current().text_muted), QLineEdit::LeadingPosition);
  header->addWidget(search_, 0, Qt::AlignTop);

  auto* segmented = new QWidget(this);
  segmented->setObjectName("segmented");
  auto* segmented_layout = new QHBoxLayout(segmented);
  segmented_layout->setContentsMargins(3, 3, 3, 3);
  segmented_layout->setSpacing(3);
  views_ = new QButtonGroup(this);
  int index = 0;
  for (const char* label : {"Added", "Add source"}) {
    auto* button = new QPushButton(label, segmented);
    button->setCheckable(true);
    views_->addButton(button, index++);
    segmented_layout->addWidget(button);
  }
  header->addWidget(segmented, 0, Qt::AlignTop);

  auto* close = new QToolButton(this);
  close->setAutoRaise(true);
  icons::Follow(close, icons::Glyph::Close);
  close->setToolTip("Close (Esc)");
  close->setAccessibleName("Close Sources");
  connect(close, &QToolButton::clicked, this, &SourcesPage::CloseRequested);
  header->addWidget(close, 0, Qt::AlignTop);
  outer->addLayout(header);
  outer->addWidget(MakeDivider(this, Qt::Horizontal));

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("card_scroll");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll, this);
  // Not a QStackedWidget: that's as tall as its tallest page, so Added would scroll as far as Add source.
  auto* views = new QWidget();
  auto* views_layout = new QVBoxLayout(views);
  views_layout->setContentsMargins(24, 16, 24, 24);
  added_ = new AddedSources(views);
  catalog_ = new SourceCatalog(views);
  catalog_->hide();
  views_layout->addWidget(added_);
  views_layout->addWidget(catalog_);
  scroll->setWidget(views);
  outer->addWidget(scroll, /*stretch=*/1);

  views_->button(0)->setChecked(true);
  connect(views_, &QButtonGroup::idClicked, this, &SourcesPage::ShowView);
  connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
    added_->SetFilter(text);
    catalog_->SetFilter(text);
  });

  connect(added_, &AddedSources::OpenRequested, this, &SourcesPage::OpenRequested);
  connect(added_, &AddedSources::SettingsRequested, this, &SourcesPage::SettingsRequested);
  connect(added_, &AddedSources::SidebarToggled, this, &SourcesPage::SidebarToggled);
  connect(added_, &AddedSources::EnabledToggled, this, &SourcesPage::EnabledToggled);
  connect(added_, &AddedSources::Imported, this, &SourcesPage::Imported);
  connect(added_, &AddedSources::Removed, this, &SourcesPage::Removed);
  connect(catalog_, &SourceCatalog::SetUpRequested, this, &SourcesPage::OpenSetup);
}

void SourcesPage::SetEntries(const std::vector<SourceEntry>& entries) {
  entries_ = entries;
  std::vector<SourceEntry> added;
  std::vector<SourceInfo> offered;
  for (const auto& entry : entries) {
    if (entry.ready) {
      added.push_back(entry);
    } else {
      offered.push_back(entry.source);
    }
  }
  added_->SetEntries(added);
  catalog_->SetSources(offered);
  views_->button(0)->setText(QString("Added  %1").arg(added.size()));
  views_->button(1)->setText(QString("Add source  %1").arg(offered.size()));
}

void SourcesPage::ShowCatalog() {
  ShowView(1);
  search_->setFocus(Qt::OtherFocusReason);
  search_->selectAll();
}

void SourcesPage::ShowAdded() { ShowView(0); }

void SourcesPage::ShowView(int view) {
  views_->button(view)->setChecked(true);
  added_->setVisible(view == 0);
  catalog_->setVisible(view == 1);
  hint_->setText(view == 1 ? kCatalogHint : kAddedHint);
}

bool SourcesPage::SetupOpen() const { return setup_overlay_ != nullptr && setup_overlay_->isVisible(); }

void SourcesPage::CloseSetup() {
  if (setup_overlay_ == nullptr) return;
  setup_overlay_->hide();
  if (QWidget* card = setup_scroll_->takeWidget()) card->deleteLater();
  setup_card_ = nullptr;
}

// Installs a newly added emulator that isn't yet, over `window`: the install outlives this page.
void SourcesPage::InstallEmulator(QWidget* window, const QString& id) {
  api::GetEmulatorsAsync(window, [window, id](EmulatorsResult result) {
    if (!result.ok) return;
    const auto it = std::ranges::find(result.emulators, id.toStdString(), &EmulatorInfo::id);
    if (it == result.emulators.end() || it->installed) return;
    // A package emulator comes from the system; asks for it the same way its page does.
    if (!it->package.empty()) {
      return system::EnsurePackages(window, it->package, QString::fromStdString(it->name), [](bool) {});
    }
    api::InstallEmulatorAsync(window, it->id, [window, name = QString::fromStdString(it->name)](StoreActionResult r) {
      if (!r.ok) notify::FailedRequest(window, "Could not install " + name + ".", r.error);
    });
  });
}

// Games an already-installed emulator had: folders its settings list, and desktop entries that run one.
// Asked over `window`, since this page closes once the emulator's opens; the library follows mirad's events.
void SourcesPage::OfferExistingGames(QWidget* window, const QString& id, const QString& name) {
  api::GetExistingEmulatorGamesAsync(window, id.toStdString(), [window, id, name](ExistingEmulatorGamesResult r) {
    if (!r.ok || (r.dirs.empty() && r.shortcuts.empty())) return;
    QStringList lines;
    for (const auto& [path, games] : r.dirs)
      lines << QString("%1 (%2 game%3)").arg(QString::fromStdString(path)).arg(games).arg(games == 1 ? "" : "s");
    for (const std::string& shortcut : r.shortcuts) lines << QString::fromStdString(shortcut) + " (desktop entry)";
    PopupDialog dialog(window, notify::Level::Info, "Games " + name + " already has");
    dialog.SetMessage("Mira found these. Move them into " + name + "'s folder, or play them from where they are?");
    dialog.SetDetail(lines.join('\n'));
    enum { kSkip, kMove, kKeep };
    dialog.AddButton("Skip", int(kSkip));
    dialog.AddButton("Keep in place", int(kKeep));
    dialog.AddButton("Move into Mira", int(kMove), /*default_button=*/true);
    const int choice = dialog.exec();
    if (choice != kMove && choice != kKeep) return;
    api::AdoptEmulatorGamesAsync(window, id.toStdString(), choice == kMove, [window, name](StoreImportResult r) {
      if (!r.ok) notify::FailedRequest(window, "Could not add " + name + "'s games.", r.error);
    });
  });
}

void SourcesPage::OpenSetup(const QString& id) {
  if (id == "lutris") {
    api::ImportLutrisAsync(this, [this, id](LutrisImportResult r) {
      if (!r.ok) {
        notify::FailedRequest(this, "Could not import Lutris games.", r.error);
        return;
      }
      emit Imported(id);
      emit OpenRequested(id);
    });
    return;
  }
  const auto entry = std::ranges::find(entries_, id, [](const SourceEntry& e) { return e.source.id; });
  if (entry == entries_.end()) return;
  const SourceInfo source = entry->source;

  // Added at once and its page opened, where it installs; one Mira downloads starts downloading now.
  if (source.kind == SourceInfo::Kind::Emulator) {
    api::AddSourceAsync(this, id.toStdString(), [this, id, name = source.name](PatchConfigResult r) {
      if (!r.ok) return notify::FailedRequest(this, "Could not add that source.", r.error);
      emit Imported(id);
      InstallEmulator(window(), id);  // before the page opens, which closes this one
      OfferExistingGames(window(), id, name);
      emit OpenRequested(id);
    });
    return;
  }

  if (setup_overlay_ == nullptr) {
    setup_overlay_ = new ModalOverlay(this);
    setup_overlay_->scrim = QColor(0, 0, 0, 150);
    setup_overlay_->setFocusPolicy(Qt::StrongFocus);
    setup_overlay_->on_backdrop_clicked = [this] { CloseSetup(); };
    setup_scroll_ = new QScrollArea(setup_overlay_);
    setup_scroll_->setWidgetResizable(true);
    setup_scroll_->setFrameShape(QFrame::NoFrame);
    setup_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    SetUpScrolling(setup_scroll_, setup_overlay_);  // set up after the page's, so it's asked first
    // Only the viewport: the card keeps its own background.
    setup_scroll_->setStyleSheet("QScrollArea, QScrollArea > QWidget { background: transparent; }");
    auto* column = new QVBoxLayout(setup_overlay_);
    column->setContentsMargins(32, 32, 32, 32);
    column->addStretch(1);
    column->addWidget(setup_scroll_, 0, Qt::AlignHCenter);
    column->addStretch(1);
  }

  setup_card_ = new SourceSetupCard(source, this);
  setup_card_->setFixedWidth(560);
  setup_card_->SetLeading(MakeSourceBadge(source, 24, setup_card_));
  auto* close = new QToolButton(setup_card_);
  icons::Follow(close, icons::Glyph::Close);
  close->setToolTip("Close");
  close->setAutoRaise(true);
  connect(close, &QToolButton::clicked, this, &SourcesPage::CloseSetup);
  setup_card_->Header()->addWidget(close);
  if (source.kind == SourceInfo::Kind::Launcher) {
    LauncherInfo launcher;
    launcher.id = id.toStdString();
    launcher.installed = false;
    setup_card_->ShowLauncher(launcher, /*installing=*/false);
  } else {
    setup_card_->ShowStore(/*tool_installed=*/true, /*authenticated=*/false);
  }
  // Done here, or already signed in from before: either way it's set up now.
  const auto added = [this, id] {
    CloseSetup();
    api::AddSourceAsync(this, id.toStdString(), [this, id](PatchConfigResult r) {
      if (!r.ok) return notify::FailedRequest(this, "Could not add that source.", r.error);
      emit Imported(id);
      emit OpenRequested(id);
    });
  };
  connect(setup_card_, &SourceSetupCard::StatusChanged, this, [this, id, added] {
    if (id != "steam") return added();
    CloseSetup();
    api::ScanSteamAsync(this, [this, id](SteamScanResult r) {
      if (!r.ok) return notify::FailedRequest(this, "Could not scan the Steam library.", r.error);
      emit Imported(id);
      emit OpenRequested(id);
    });
  });
  connect(setup_card_, &SourceSetupCard::LauncherInstallStarted, this, added);

  setup_scroll_->setWidget(setup_card_);
  setup_card_->installEventFilter(this);  // the panel grows as mirad answers
  setup_card_->show();
  setup_overlay_->setGeometry(rect());
  FitSetup();
  setup_overlay_->show();
  setup_overlay_->raise();
  setup_overlay_->setFocus();
}

// The card at its natural size, 560 wide at most, shrunk to fit a small window.
void SourcesPage::FitSetup() {
  if (setup_card_ == nullptr) return;
  constexpr int kMargin = 32;
  const int width = std::min(560, std::max(280, this->width() - 2 * kMargin));
  setup_card_->setFixedWidth(width);
  const int wanted = setup_card_->heightForWidth(width) > 0 ? setup_card_->heightForWidth(width)
                                                            : setup_card_->sizeHint().height();
  setup_scroll_->setFixedWidth(width + (wanted > height() - 2 * kMargin ? setup_scroll_->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0));
  setup_scroll_->setFixedHeight(std::min(wanted, std::max(120, height() - 2 * kMargin)));
}

bool SourcesPage::eventFilter(QObject* watched, QEvent* event) {
  if (watched == setup_card_ && event->type() == QEvent::LayoutRequest && SetupOpen()) {
    QMetaObject::invokeMethod(this, &SourcesPage::FitSetup, Qt::QueuedConnection);
  }
  return QFrame::eventFilter(watched, event);
}

void SourcesPage::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  if (setup_overlay_ != nullptr) {
    setup_overlay_->setGeometry(rect());
    FitSetup();
  }
}

}  // namespace mira_gui
