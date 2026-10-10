#include "SourcesPage.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../widgets/Scrolling.h"
#include "SourceCatalog.h"
#include "SourceText.h"

namespace mira_gui {

SourcesPage::SourcesPage(QWidget* parent) : QWidget(parent) {
  setObjectName("sources_page");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* header_box = new QWidget(this);
  header_box->setObjectName("settings_canvas");
  auto* header = new QHBoxLayout(header_box);
  header->setContentsMargins(32, 22, 32, 6);
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
  column->setMaximumWidth(760);
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
  connect(catalog_, &SourceCatalog::SetUpRequested, this, &SourcesPage::OpenRequested);
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

}  // namespace mira_gui
