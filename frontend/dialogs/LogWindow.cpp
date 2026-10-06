#include "LogWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <map>

#include "../app/ErrorHelp.h"
#include "../client/api/Logs.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"

namespace mira_gui {
namespace {

constexpr int kKeepLines = 20000;
constexpr int kFirstLines = 500;

std::map<QString, QPointer<LogWindow>>& Windows() {
  static std::map<QString, QPointer<LogWindow>> windows;
  return windows;
}

}  // namespace

void LogWindow::Open(QWidget* parent, const QString& channel, const QString& title) {
  QPointer<LogWindow>& existing = Windows()[channel];
  if (existing == nullptr) existing = new LogWindow(parent != nullptr ? parent->window() : nullptr, channel, title);
  if (!existing->isVisible() && parent != nullptr) {
    // Beside the window it came from, offset a little per log so several don't sit on each other.
    const QRect from = parent->window()->frameGeometry();
    const int stagger = static_cast<int>(Windows().size() - 1) * 28;
    existing->move(from.center() - QPoint(existing->width() / 2, existing->height() / 2) + QPoint(stagger, stagger));
  }
  existing->show();
  existing->setWindowState(existing->windowState() & ~Qt::WindowMinimized);
  existing->raise();
  existing->activateWindow();
}

LogWindow::LogWindow(QWidget* parent, QString channel, QString title)
    : QWidget(parent, Qt::Window), channel_(std::move(channel)), title_(std::move(title)) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(title_ + " · Log");
  resize(860, 560);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(18, 14, 18, 12);
  root->setSpacing(10);

  auto* head = new QHBoxLayout();
  auto* name = new QLabel(title_, this);
  name->setProperty("role", "heading");
  head->addWidget(name);
  state_ = new QLabel(this);
  state_->setTextFormat(Qt::RichText);
  head->addWidget(state_);
  head->addStretch(1);
  root->addLayout(head);

  auto* tools = new QHBoxLayout();
  tools->setSpacing(8);
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText("Filter lines…");
  filter_->setClearButtonEnabled(true);
  filter_->setMaximumWidth(260);
  connect(filter_, &QLineEdit::textChanged, this, &LogWindow::Render);
  tools->addWidget(filter_);
  tools->addStretch(1);
  follow_ = new QCheckBox("Follow", this);
  follow_->setChecked(true);
  follow_->setToolTip("Keep the newest line in view");
  tools->addWidget(follow_);
  wrap_ = new QCheckBox("Wrap lines", this);
  connect(wrap_, &QCheckBox::toggled, this,
          [this](bool on) { view_->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap); });
  tools->addWidget(wrap_);
  pause_ = new QPushButton("Pause", this);
  connect(pause_, &QPushButton::clicked, this, [this] {
    paused_ = !paused_;
    pause_->setText(paused_ ? "Resume" : "Pause");
    UpdateStatus();
    if (!paused_) Poll();
  });
  tools->addWidget(pause_);
  auto* copy = new QPushButton("Copy", this);
  connect(copy, &QPushButton::clicked, this, [this] {
    QString selected = view_->textCursor().selectedText();
    selected.replace(QChar(0x2029), '\n');
    QApplication::clipboard()->setText(selected.isEmpty() ? view_->toPlainText() : selected);
  });
  tools->addWidget(copy);
  auto* save = new QPushButton("Save…", this);
  connect(save, &QPushButton::clicked, this, &LogWindow::Save);
  tools->addWidget(save);
  auto* clear = new QPushButton("Clear", this);
  clear->setToolTip("Empties this window; the log itself is untouched");
  connect(clear, &QPushButton::clicked, this, &LogWindow::Clear);
  tools->addWidget(clear);
  root->addLayout(tools);

  view_ = new QPlainTextEdit(this);
  view_->setObjectName("log_view");
  view_->setReadOnly(true);
  view_->setLineWrapMode(QPlainTextEdit::NoWrap);
  view_->setMaximumBlockCount(kKeepLines);
  QFont font("monospace");
  font.setStyleHint(QFont::Monospace);
  view_->setFont(font);
  root->addWidget(view_, 1);

  count_ = new QLabel(this);
  count_->setProperty("role", "subtle");
  root->addWidget(count_);

  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, &LogWindow::Poll);
  timer_->start(1000);
  UpdateStatus();
  Poll();
}

void LogWindow::closeEvent(QCloseEvent* event) {
  Windows().erase(channel_);
  QWidget::closeEvent(event);
}

void LogWindow::Poll() {
  if (polling_ || paused_) return;
  polling_ = true;
  api::GetLogAsync(this, channel_.toStdString(), cursor_, kFirstLines, [this](LogResult result) {
    polling_ = false;
    failed_ = !result.ok;
    if (!result.ok) {
      UpdateStatus();
      return;
    }
    // A log that started over (a game launched again): what was shown is from the last run.
    if (cursor_ && result.next < *cursor_) Clear();
    cursor_ = result.next;
    active_ = result.active;
    Add(result.lines);
    UpdateStatus();
    // Quieter once nothing is writing; it still notices a restart.
    timer_->setInterval(active_ ? 700 : 3000);
  });
}

void LogWindow::Add(const std::vector<std::string>& lines) {
  QScrollBar* bar = view_->verticalScrollBar();
  const bool at_end = bar->value() >= bar->maximum() - 4;
  for (const std::string& raw : lines) {
    QString line = QString::fromStdString(raw);
    lines_.push_back(line);
    if (static_cast<int>(lines_.size()) > kKeepLines) lines_.erase(lines_.begin());
    if (filter_->text().isEmpty() || line.contains(filter_->text(), Qt::CaseInsensitive)) AppendLine(line);
  }
  if (!lines.empty() && follow_->isChecked() && at_end) bar->setValue(bar->maximum());
  count_->setText(QString("%1 lines").arg(lines_.size()));
}

void LogWindow::AppendLine(const QString& line) {
  const theme::Tokens& tokens = theme::Current();
  QTextCharFormat format;
  const QString lower = line.toLower();
  if (lower.contains("error") || lower.contains("failed") || lower.contains("fatal")) {
    format.setForeground(tokens.error);
  } else if (lower.contains("warn")) {
    format.setForeground(tokens.warning);
  } else {
    format.setForeground(tokens.text);
  }
  QTextCursor cursor(view_->document());
  cursor.movePosition(QTextCursor::End);
  if (!view_->document()->isEmpty()) cursor.insertBlock();
  cursor.insertText(line, format);
}

void LogWindow::Clear() {
  lines_.clear();
  view_->clear();
  count_->setText("0 lines");
}

void LogWindow::Render() {
  view_->clear();
  const QString needle = filter_->text();
  for (const QString& line : lines_) {
    if (needle.isEmpty() || line.contains(needle, Qt::CaseInsensitive)) AppendLine(line);
  }
  view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
}

void LogWindow::UpdateStatus() {
  const theme::Tokens& tokens = theme::Current();
  const auto dot = [](const QColor& color) { return QString("<span style=\"color:%1\">●</span> ").arg(color.name()); };
  if (failed_) {
    state_->setText(dot(tokens.error) + "Can't reach Mira's service");
  } else if (paused_) {
    state_->setText(dot(tokens.warning) + "Paused");
  } else if (active_) {
    state_->setText(dot(tokens.success) + "Live");
  } else {
    state_->setText(dot(tokens.text_muted) + "Not running");
  }
}

void LogWindow::Save() {
  const QString path = QFileDialog::getSaveFileName(
      this, "Save log", QDir::homePath() + "/" + title_.toLower().replace(' ', '-') + ".log", "Log files (*.log *.txt)");
  if (path.isEmpty()) return;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  for (const QString& line : lines_) file.write(line.toUtf8() + '\n');
}

}  // namespace mira_gui
