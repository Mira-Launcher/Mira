#include "DownloadsPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../components/Button.h"
#include "../components/Metrics.h"
#include "../components/Scrolling.h"
#include "../components/Text.h"
#include "DownloadList.h"
#include "DownloadTracker.h"

namespace mira_gui {

DownloadsPanel::DownloadsPanel(DownloadTracker* tracker, ArtworkStore* artwork, QWidget* parent)
    : Popover(parent), tracker_(tracker) {
  setFixedWidth(380);
  // Laid out as a settings card: titled header, then rows split by a line.
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 6);
  layout->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(kRowPaddingX, 12, 10, 6);
  auto* title = new QLabel("Activity", this);
  SetTextStyle(title, text::kSection);
  header->addWidget(title);
  header->addStretch(1);
  auto* manage = new Button("All downloads", ButtonKind::Link, this);
  connect(manage, &QPushButton::clicked, this, [this] {
    hide();
    emit ManageRequested();
  });
  header->addWidget(manage);
  clear_ = new Button("Clear finished", ButtonKind::Link, this);
  connect(clear_, &QPushButton::clicked, tracker_, &DownloadTracker::ClearFinished);
  connect(clear_, &QPushButton::clicked, this, &DownloadsPanel::FinishedCleared);
  header->addWidget(clear_);
  layout->addLayout(header);

  empty_ = new QLabel("Nothing installing, downloading or running", this);
  SetTextStyle(empty_, text::kSubtle);
  empty_->setContentsMargins(kRowPaddingX, 6, kRowPaddingX, 10);
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
  PopUpBelow(anchor, popup::Align::End);
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
