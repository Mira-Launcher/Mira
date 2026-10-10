#include "SourcesPage.h"

#include <QButtonGroup>
#include <QEvent>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Library.h"
#include "../theme/Icons.h"
#include "../widgets/ModalOverlay.h"
#include "../widgets/Scrolling.h"
#include "SourceCatalog.h"
#include "SourceSetupCard.h"
#include "SourceText.h"
#include "Sources.h"

namespace mira_gui {

namespace {
constexpr int kColumnWidth = 760;
}  // namespace

SourcesPage::SourcesPage(QWidget* parent) : QWidget(parent) {
  setObjectName("sources_page");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* header_box = new QWidget(this);
  header_box->setObjectName("settings_canvas");
  auto* header_row = new QHBoxLayout(header_box);
  header_row->setContentsMargins(32, 22, 32, 6);
  // As wide as the column below, so the switch ends where the cards do.
  auto* header_column = new QWidget(header_box);
  header_column->setMaximumWidth(kColumnWidth);
  header_row->addWidget(header_column, /*stretch=*/1);
  auto* header = new QHBoxLayout(header_column);
  header->setContentsMargins(0, 0, 0, 0);
  auto* title = new QLabel("Sources", this);
  title->setObjectName("page_title");
  header->addWidget(title, /*stretch=*/1);
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
  header->addWidget(segmented);
  outer->addWidget(header_box);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("settings_page");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll, this);
  auto* canvas = new QWidget();
  canvas->setObjectName("settings_canvas");
  auto* canvas_layout = new QVBoxLayout(canvas);
  canvas_layout->setContentsMargins(32, 12, 32, 22);
  // Both views share this column, so they line up with each other and the header.
  auto* column = new QWidget(canvas);
  column->setMaximumWidth(kColumnWidth);
  auto* column_layout = new QVBoxLayout(column);
  column_layout->setContentsMargins(0, 0, 0, 0);
  stack_ = new QStackedWidget(column);
  column_layout->addWidget(stack_);
  canvas_layout->addWidget(column);
  canvas_layout->addStretch(1);
  scroll->setWidget(canvas);
  outer->addWidget(scroll, /*stretch=*/1);

  auto* added = new QWidget(stack_);
  auto* added_layout = new QVBoxLayout(added);
  added_layout->setContentsMargins(0, 0, 0, 0);
  added_layout->setSpacing(18);
  games_ = new ManageSourcesCard("Game sources", added);
  apps_ = new ManageSourcesCard("App sources", added);
  added_layout->addWidget(games_);
  added_layout->addWidget(apps_);
  added_layout->addStretch(1);
  stack_->addWidget(added);
  catalog_ = new SourceCatalog(stack_);
  stack_->addWidget(catalog_);

  views_->button(0)->setChecked(true);
  connect(views_, &QButtonGroup::idClicked, this, [this](int view) {
    if (view == 1) {
      ShowCatalog();
    } else {
      ShowAdded();
    }
  });

  for (ManageSourcesCard* card : {games_, apps_}) {
    connect(card, &ManageSourcesCard::OpenRequested, this, &SourcesPage::OpenRequested);
    connect(card, &ManageSourcesCard::SettingsRequested, this, &SourcesPage::SettingsRequested);
    connect(card, &ManageSourcesCard::SidebarToggled, this, &SourcesPage::SidebarToggled);
    connect(card, &ManageSourcesCard::EnabledToggled, this, &SourcesPage::EnabledToggled);
    connect(card, &ManageSourcesCard::Imported, this, &SourcesPage::Imported);
    connect(card, &ManageSourcesCard::Removed, this, &SourcesPage::Removed);
    // The card only reorders its own sources; the others keep their slots.
    connect(card, &ManageSourcesCard::OrderChanged, this, [this](const QStringList& ids) {
      QStringList full;
      int next = 0;
      for (const auto& entry : entries_) {
        const QString id = entry.source.id;
        full << (ids.contains(id) ? ids.at(next++) : id);
      }
      emit OrderChanged(full);
    });
  }
  connect(catalog_, &SourceCatalog::SetUpRequested, this, &SourcesPage::OpenSetup);
}

void SourcesPage::SetEntries(const std::vector<ManageSourcesCard::Entry>& entries) {
  entries_ = entries;
  std::vector<ManageSourcesCard::Entry> games;
  std::vector<ManageSourcesCard::Entry> apps;
  std::vector<SourceInfo> offered;
  for (const auto& entry : entries) {
    if (!entry.ready) {
      offered.push_back(entry.source);
    } else if (CopyFor(entry.source.id.toStdString()).item == "app") {
      apps.push_back(entry);
    } else {
      games.push_back(entry);
    }
  }
  games_->SetEntries(games);
  apps_->SetEntries(apps);
  apps_->setVisible(!apps.empty());
  catalog_->SetSources(offered);
}

void SourcesPage::ShowCatalog() {
  views_->button(1)->setChecked(true);
  stack_->setCurrentIndex(1);
  catalog_->FocusSearch();
}

void SourcesPage::ShowAdded() {
  views_->button(0)->setChecked(true);
  stack_->setCurrentIndex(0);
}

bool SourcesPage::SetupOpen() const { return setup_overlay_ != nullptr && setup_overlay_->isVisible(); }

void SourcesPage::CloseSetup() {
  if (setup_overlay_ == nullptr) return;
  setup_overlay_->hide();
  if (QWidget* card = setup_scroll_->takeWidget()) card->deleteLater();
  setup_card_ = nullptr;
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
  const auto entry = std::ranges::find(entries_, id, [](const ManageSourcesCard::Entry& e) { return e.source.id; });
  if (entry == entries_.end()) return;
  const SourceInfo source = entry->source;

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
  connect(setup_card_, &SourceSetupCard::StatusChanged, this, [this, id] {
    if (id != "steam") {
      CloseSetup();
      emit OpenRequested(id);
      return;
    }
    CloseSetup();
    api::ScanSteamAsync(this, [this, id](SteamScanResult r) {
      if (!r.ok) return notify::FailedRequest(this, "Could not scan the Steam library.", r.error);
      emit Imported(id);
      emit OpenRequested(id);
    });
  });
  connect(setup_card_, &SourceSetupCard::LauncherInstallStarted, this, [this, id] {
    CloseSetup();
    emit OpenRequested(id);
  });

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
  return QWidget::eventFilter(watched, event);
}

void SourcesPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (setup_overlay_ != nullptr) {
    setup_overlay_->setGeometry(rect());
    FitSetup();
  }
}

}  // namespace mira_gui
