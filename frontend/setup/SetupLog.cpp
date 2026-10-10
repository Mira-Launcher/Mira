#include "SetupLog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include "../client/api/Logs.h"
#include "../dialogs/LogWindow.h"

namespace mira_gui {

SetupLog::SetupLog(const std::string& id, const QString& name, QWidget* parent)
    : QWidget(parent), id_(id) {
  setVisible(false);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  tail_ = new QLabel(this);
  tail_->setObjectName("setup_log_tail");
  tail_->setWordWrap(true);
  tail_->setTextFormat(Qt::PlainText);
  tail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont mono("monospace");
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSizeF(mono.pointSizeF() * 0.9);
  tail_->setFont(mono);
  tail_->setProperty("role", "muted");
  layout->addWidget(tail_);
  auto* open = new QPushButton("View full log", this);
  open->setObjectName("text_button");
  const QString channel = QString::fromStdString("setup:" + id);
  connect(open, &QPushButton::clicked, this,
          [this, channel, name] { LogWindow::Open(this, channel, name + " setup"); });
  auto* row = new QHBoxLayout();
  row->addWidget(open);
  row->addStretch(1);
  layout->addLayout(row);
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, &SetupLog::Poll);
}

void SetupLog::Watch(bool on) {
  if (on) {
    setVisible(true);
    if (!timer_->isActive()) timer_->start(1500);
    Poll();
  } else {
    timer_->stop();
    if (isVisible()) Poll();
  }
}

void SetupLog::ShowLast() {
  timer_->stop();
  setVisible(true);
  Poll();
}

void SetupLog::Poll() {
  if (busy_) return;
  busy_ = true;
  api::GetLogAsync(this, "setup:" + id_, std::nullopt, 6, [this](LogResult result) {
    busy_ = false;
    if (!result.ok) return;
    QStringList lines;
    for (const std::string& line : result.lines) lines << QString::fromStdString(line);
    if (!result.live.empty()) lines << QString::fromStdString(result.live);
    while (lines.size() > 6) lines.removeFirst();
    static const QRegularExpression percent_line(R"(^\s*(\d+(?:\.\d+)?)%)");
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
      const QRegularExpressionMatch match = percent_line.match(*line);
      if (!match.hasMatch()) continue;
      emit Progress(match.captured(1).toDouble() / 100);
      break;
    }
    tail_->setText(lines.isEmpty() ? QString("Waiting for output…") : lines.join('\n'));
  });
}

}  // namespace mira_gui
