#include "PasswordPopup.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "../client/Types.h"
#include "../widgets/Labels.h"
#include "LinkDownloads.h"

namespace mira_gui {

PasswordPopup::PasswordPopup(LinkDownloads* downloads, QWidget* parent)
    : QWidget(parent, Qt::Popup), downloads_(downloads) {
  setObjectName("updates_popover");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(340);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(8);

  name_ = new ElidedLabel(QString(), this);
  name_->setStyleSheet("font-weight: 600;");
  layout->addWidget(name_);
  auto* note = MakeLabel(
      this, "The saved passwords didn't open this archive. Mira keeps the one that works.",
      "muted");
  note->setWordWrap(true);
  layout->addWidget(note);

  password_ = new QLineEdit(this);
  password_->setPlaceholderText("Password");
  layout->addWidget(password_);
  error_ = MakeLabel(this, "That password didn't open it.", "error");
  layout->addWidget(error_);

  auto* buttons = new QHBoxLayout();
  count_ = MakeLabel(this, QString(), "muted");
  buttons->addWidget(count_);
  buttons->addStretch(1);
  auto* skip = new QPushButton("Skip", this);
  skip->setObjectName("text_button");
  skip->setProperty("tone", "muted");
  connect(skip, &QPushButton::clicked, this, [this] {
    if (!current_.empty()) downloads_->Skip(current_);
  });
  buttons->addWidget(skip);
  extract_ = new QPushButton("Extract", this);
  extract_->setDefault(true);
  connect(extract_, &QPushButton::clicked, this, &PasswordPopup::Submit);
  connect(password_, &QLineEdit::returnPressed, this, &PasswordPopup::Submit);
  connect(password_, &QLineEdit::textChanged, this,
          [this](const QString& text) { extract_->setEnabled(!text.isEmpty()); });
  buttons->addWidget(extract_);
  layout->addLayout(buttons);

  connect(downloads_, &LinkDownloads::NoticeChanged, this, [this] {
    if (isVisible()) Refresh();
  });
}

void PasswordPopup::ShowBelow(QWidget* anchor, const std::string& id) {
  current_ = id;
  Refresh();
  if (current_.empty()) return;
  const QPoint corner = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 4));
  move(corner.x() - width(), corner.y());
  show();
  password_->setFocus();
}

void PasswordPopup::Refresh() {
  const auto waiting = downloads_->WaitingForPassword();
  const auto chosen = std::ranges::find(waiting, current_, &DownloadInfo::id);
  if (waiting.empty()) {
    current_.clear();
    hide();
    return;
  }
  const DownloadInfo& download = chosen == waiting.end() ? waiting.front() : *chosen;
  current_ = download.id;
  if (std::exchange(shown_, current_) != current_) password_->clear();
  name_->setText(QString::fromStdString(download.name));
  name_->setToolTip(name_->text());
  count_->setText(waiting.size() > 1 ? QString("%1 waiting").arg(waiting.size()) : QString());
  error_->setVisible(download.error.code == "wrong_password");
  extract_->setEnabled(!password_->text().isEmpty());
  adjustSize();
}

void PasswordPopup::Submit() {
  const std::string password = password_->text().toStdString();
  if (current_.empty() || password.empty()) return;
  extract_->setEnabled(false);
  downloads_->SubmitPassword(current_, password);
  password_->clear();
  hide();
}

}  // namespace mira_gui
