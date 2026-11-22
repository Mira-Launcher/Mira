#include "DownloadsPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../components/Scrolling.h"
#include "DownloadList.h"
#include "DownloadTracker.h"

namespace mira_gui {

DownloadsPanel::DownloadsPanel(DownloadTracker* tracker, ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent, Qt::Popup), tracker_(tracker) {
  setObjectName("downloads_popover");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(380);
  // Laid out as a settings card: titled header, then rows split by a line.
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 6);
  layout->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(18, 12, 10, 6);
  auto* title = new QLabel("Activity", this);
  title->setProperty("role", "section");
  header->addWidget(title);
  header->addStretch(1);
  auto* manage = new QPushButton("All downloads", this);
  manage->setObjectName("text_button");
  connect(manage, &QPushButton::clicked, this, [this] {
    hide();
    emit ManageRequested();
  });
  header->addWidget(manage);
  clear_ = new QPushButton("Clear finished", this);
  clear_->setObjectName("text_button");
  connect(clear_, &QPushButton::clicked, tracker_, &DownloadTracker::ClearFinished);
  connect(clear_, &QPushButton::clicked, this, &DownloadsPanel::FinishedCleared);
  header->addWidget(clear_);
  layout->addLayout(header);

  empty_ = new QLabel("Nothing installing, downloading or running", this);
  empty_->setProperty("role", "subtle");
  empty_->setContentsMargins(18, 6, 18, 10);
  layout->addWidget(empty_);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("downloads_scroll");
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll_, this);
  list_ = new DownloadList(tracker_, artwork, DownloadList::Style::Compact, scroll_);
  scroll_->setWidget(list_);
  layout->addWidget(scroll_);

  connect(list_, &DownloadList::Rebuilt, this, &DownloadsPanel::Fit);
  connect(list_, &DownloadList::Leaving, this, &QWidget::hide);
  connect(list_, &DownloadList::ShowGameRequested, this, &DownloadsPanel::ShowGameRequested);
  connect(list_, &DownloadList::LogRequested, this, &DownloadsPanel::LogRequested);
  connect(list_, &DownloadList::LinkAction, this, &DownloadsPanel::LinkAction);
  connect(list_, &DownloadList::LinkPasswordRequested, this,
          &DownloadsPanel::LinkPasswordRequested);
}

void DownloadsPanel::ShowBelow(QWidget* anchor) {
  list_->Rebuild();
  const QPoint corner = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 4));
  move(corner.x() - width(), corner.y());
  show();
}

void DownloadsPanel::Fit() {
  const int count = list_->Count();
  empty_->setVisible(count == 0);
  clear_->setEnabled(tracker_->RunningCount() < static_cast<int>(tracker_->Entries().size()));
  // Grows with its rows up to six, then scrolls. Measured at the popover's width, since a
  // failed row's message and hint wrap onto more lines than a cover's height.
  scroll_->setVisible(count > 0);
  scroll_->setFixedHeight(list_->HeightFor(width(), 6));
  adjustSize();
}

}  // namespace mira_gui
