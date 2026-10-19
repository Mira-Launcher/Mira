#include "LogWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <map>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/api/Logs.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../window/FramelessRoot.h"

namespace mira_gui {
namespace {

constexpr int kKeepLines = 20000;
constexpr int kFirstLines = 500;

std::map<QString, QPointer<LogWindow>>& Windows() {
  static std::map<QString, QPointer<LogWindow>> windows;
  return windows;
}

void ToggleMaximized(QWidget* window) {
  window->isMaximized() ? window->showNormal() : window->showMaximized();
}

// The window's own title bar: dragging it moves the window, a double click maximizes it.
class LogBar : public QWidget {
 public:
  explicit LogBar(QWidget* parent) : QWidget(parent) {
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

}  // namespace

void LogWindow::Open(QWidget* parent, const QString& channel, const QString& title) {
  QPointer<LogWindow>& existing = Windows()[channel];
  if (existing == nullptr)
    existing = new LogWindow(parent != nullptr ? parent->window() : nullptr, channel, title);
  if (!existing->isVisible() && parent != nullptr) {
    // Beside the window it came from, offset a little per log so several don't sit on each other.
    const QRect from = parent->window()->frameGeometry();
    const int stagger = static_cast<int>(Windows().size() - 1) * 28;
    existing->move(from.center() - QPoint(existing->width() / 2, existing->height() / 2) +
                   QPoint(stagger, stagger));
  }
  existing->show();
  existing->setWindowState(existing->windowState() & ~Qt::WindowMinimized);
  existing->raise();
  existing->activateWindow();
}

LogWindow::LogWindow(QWidget* parent, QString channel, QString title)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint),
      channel_(std::move(channel)),
      title_(std::move(title)) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(title_ + " · Log");
  resize(860, 560);
  new QShortcut(QKeySequence(Qt::Key_Escape), this, this, &QWidget::close);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* frame = new FramelessRoot(this);
  // The window's edge, which a frameless window doesn't get from the system.
  frame->setObjectName("log_frame");
  frame->setAttribute(Qt::WA_StyledBackground);
  outer->addWidget(frame);
  auto* root = new QVBoxLayout(frame);
  root->setContentsMargins(kResizeMargin, kResizeMargin, kResizeMargin, kResizeMargin);
  root->setSpacing(0);

  // Mira's own bar: what the log is and whether it's live, then the window's buttons.
  auto* bar = new LogBar(frame);
  bar->setCursor(Qt::ArrowCursor);
  auto* bar_layout = new QHBoxLayout(bar);
  bar_layout->setContentsMargins(12, 4, 6, 4);
  bar_layout->setSpacing(10);
  auto* badge = new QLabel("M", bar);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  bar_layout->addWidget(badge);
  auto* name = new QLabel(title_, bar);
  name->setObjectName("log_title");
  bar_layout->addWidget(name);
  state_ = new QLabel(bar);
  state_->setTextFormat(Qt::RichText);
  bar_layout->addWidget(state_);
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

  auto* body = new QWidget(frame);
  body->setCursor(Qt::ArrowCursor);
  auto* body_layout = new QVBoxLayout(body);
  body_layout->setContentsMargins(16, 10, 16, 8);
  body_layout->setSpacing(8);
  auto* tools = new QHBoxLayout();
  tools->setSpacing(6);
  filter_ = new QLineEdit(body);
  filter_->setPlaceholderText("Filter");
  filter_->setClearButtonEnabled(true);
  filter_->setMaximumWidth(240);
  // Each render redraws every kept line, so it waits for a pause in typing.
  auto* refilter = new QTimer(this);
  refilter->setSingleShot(true);
  refilter->setInterval(150);
  connect(filter_, &QLineEdit::textChanged, refilter, qOverload<>(&QTimer::start));
  connect(refilter, &QTimer::timeout, this, &LogWindow::Render);
  tools->addWidget(filter_);
  tools->addStretch(1);
  const auto toggle = [body, tools](const QString& text, const QString& tooltip) {
    auto* button = new QPushButton(text, body);
    button->setObjectName("chip");
    button->setCheckable(true);
    button->setToolTip(tooltip);
    tools->addWidget(button);
    return button;
  };
  follow_ = toggle("Follow", "Keep the newest line in view");
  follow_->setChecked(true);
  wrap_ = toggle("Wrap", "Wrap long lines");
  connect(wrap_, &QPushButton::toggled, this, [this](bool on) {
    view_->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  });
  pause_ = toggle("Pause", "Stop reading new lines until it's switched off");
  connect(pause_, &QPushButton::toggled, this, [this](bool on) {
    paused_ = on;
    UpdateStatus();
    if (!paused_) Poll();
  });
  const auto tool = [body, tools](const QString& tooltip) {
    auto* button = new QToolButton(body);
    button->setAutoRaise(true);
    button->setToolTip(tooltip);
    tools->addWidget(button);
    return button;
  };
  copy_ = tool("Copy the selection, or the whole log");
  connect(copy_, &QToolButton::clicked, this, [this] {
    QString selected = view_->textCursor().selectedText();
    selected.replace(QChar(0x2029), '\n');
    QApplication::clipboard()->setText(selected.isEmpty() ? view_->toPlainText() : selected);
    // A tick for a moment says it worked, without a notice.
    copied_ = true;
    ApplyIcons();
    QTimer::singleShot(1500, copy_, [this] {
      copied_ = false;
      ApplyIcons();
    });
  });
  save_ = tool("Save to a file…");
  connect(save_, &QToolButton::clicked, this, &LogWindow::Save);
  clear_ = tool("Empty this window; the log itself is untouched");
  connect(clear_, &QToolButton::clicked, this, &LogWindow::Clear);
  body_layout->addLayout(tools);

  view_ = new QPlainTextEdit(body);
  view_->setObjectName("log_view");
  view_->setReadOnly(true);
  view_->setLineWrapMode(QPlainTextEdit::NoWrap);
  view_->setMaximumBlockCount(kKeepLines);
  QFont font("monospace");
  font.setStyleHint(QFont::Monospace);
  view_->setFont(font);
  body_layout->addWidget(view_, 1);

  count_ = new QLabel(body);
  count_->setProperty("role", "subtle");
  body_layout->addWidget(count_);
  root->addWidget(body, 1);
  ApplyIcons();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    ApplyIcons();
    UpdateStatus();
  });

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

void LogWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::WindowStateChange) ApplyIcons();
  QWidget::changeEvent(event);
}

void LogWindow::ApplyIcons() {
  using icons::Glyph;
  minimize_->setIcon(icons::For(Glyph::Minimize));
  maximize_->setIcon(icons::For(isMaximized() ? Glyph::Restore : Glyph::Maximize));
  maximize_->setToolTip(isMaximized() ? "Restore" : "Maximize");
  close_->setIcon(icons::For(Glyph::Close));
  copy_->setIcon(copied_ ? icons::For(Glyph::CheckCircle, theme::Current().success)
                         : icons::For(Glyph::Copy));
  save_->setIcon(icons::For(Glyph::Download));
  clear_->setIcon(icons::For(Glyph::Trash));
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
    RemoveLive();
    Add(result.lines);
    ShowLive(QString::fromStdString(result.live));
    UpdateStatus();
    // Quieter once nothing is writing; it still notices a restart.
    timer_->setInterval(active_ ? 700 : 3000);
  });
}

bool LogWindow::Matches(const QString& line) const {
  return filter_->text().isEmpty() || line.contains(filter_->text(), Qt::CaseInsensitive);
}

// All the kept lines, or how many of them the filter shows.
void LogWindow::UpdateCount() {
  const QLocale locale;
  const QString total = locale.toString(static_cast<qlonglong>(lines_.size()));
  count_->setText(filter_->text().isEmpty()
                      ? QString("%1 lines").arg(total)
                      : QString("%1 of %2 lines").arg(locale.toString(shown_), total));
}

void LogWindow::Add(const std::vector<std::string>& lines) {
  QScrollBar* bar = view_->verticalScrollBar();
  const bool at_end = bar->value() >= bar->maximum() - 4;
  for (const std::string& raw : lines) {
    QString line = QString::fromStdString(raw);
    lines_.push_back(line);
    if (static_cast<int>(lines_.size()) > kKeepLines) {
      if (Matches(lines_.front())) --shown_;
      lines_.pop_front();
    }
    if (Matches(line)) {
      ++shown_;
      AppendLine(line);
    }
  }
  if (!lines.empty() && follow_->isChecked() && at_end) bar->setValue(bar->maximum());
  UpdateCount();
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

void LogWindow::RemoveLive() {
  if (!has_live_) return;
  has_live_ = false;
  if (view_->document()->blockCount() <= 1) {
    view_->clear();
    return;
  }
  QTextCursor cursor(view_->document());
  cursor.movePosition(QTextCursor::End);
  cursor.select(QTextCursor::BlockUnderCursor);
  cursor.removeSelectedText();
}

void LogWindow::ShowLive(const QString& text) {
  live_ = text;
  if (text.isEmpty() || !Matches(text)) return;
  QScrollBar* bar = view_->verticalScrollBar();
  const bool at_end = bar->value() >= bar->maximum() - 4;
  AppendLine(text);
  has_live_ = true;
  if (follow_->isChecked() && at_end) bar->setValue(bar->maximum());
}

void LogWindow::Clear() {
  lines_.clear();
  has_live_ = false;
  live_.clear();
  view_->clear();
  shown_ = 0;
  UpdateCount();
}

void LogWindow::Render() {
  view_->clear();
  has_live_ = false;
  shown_ = 0;
  for (const QString& line : lines_) {
    if (!Matches(line)) continue;
    ++shown_;
    AppendLine(line);
  }
  ShowLive(live_);
  UpdateCount();
  view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
}

void LogWindow::UpdateStatus() {
  const theme::Tokens& tokens = theme::Current();
  if (failed_) {
    state_->setText(StatusDot(tokens.error) + "Can't reach Mira's service");
  } else if (paused_) {
    state_->setText(StatusDot(tokens.warning) + "Paused");
  } else if (active_) {
    state_->setText(StatusDot(tokens.success) + "Live");
  } else {
    state_->setText(StatusDot(tokens.text_muted) + "Not running");
  }
}

void LogWindow::Save() {
  const QString path = QFileDialog::getSaveFileName(
      this, "Save log", QDir::homePath() + "/" + title_.toLower().replace(' ', '-') + ".log",
      "Log files (*.log *.txt)");
  if (path.isEmpty()) return;
  QFile file(path);
  QByteArray text;
  for (const QString& line : lines_) text += line.toUtf8() + '\n';
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text) != text.size())
    notify::Info(this, "Could not save the log", file.errorString());
}

}  // namespace mira_gui
