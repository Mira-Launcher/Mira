#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace mira_gui {

// A task's live log in a window of its own (GET /v1/logs/<channel>). One window
// per channel: opening the same log again raises it, opening another task's
// opens a second window, so logs never mix.
class LogWindow : public QWidget {
  Q_OBJECT

public:
  // `title` names the task ("Microsoft 365 setup"). The window belongs to `parent`'s window but is its own.
  static void Open(QWidget* parent, const QString& channel, const QString& title);

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  LogWindow(QWidget* parent, QString channel, QString title);

  void Poll();
  void Add(const std::vector<std::string>& lines);
  void Clear();
  void Render();
  void AppendLine(const QString& line);
  // The line being redrawn (a download's progress) sits last and is replaced in place, never repeated.
  void RemoveLive();
  void ShowLive(const QString& text);
  void UpdateStatus();
  void Save();

  QString channel_;
  QString title_;
  QPlainTextEdit* view_ = nullptr;
  QLabel* state_ = nullptr;
  QLabel* count_ = nullptr;
  QLineEdit* filter_ = nullptr;
  QPushButton* pause_ = nullptr;
  QCheckBox* wrap_ = nullptr;
  QCheckBox* follow_ = nullptr;
  QTimer* timer_ = nullptr;

  std::vector<QString> lines_;
  std::optional<std::uint64_t> cursor_;
  bool active_ = false;
  bool paused_ = false;
  bool polling_ = false;
  bool failed_ = false;
  bool has_live_ = false;
  QString live_;
};

}  // namespace mira_gui
