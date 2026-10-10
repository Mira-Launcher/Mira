#pragma once

#include <QString>
#include <QWidget>
#include <string>

class QLabel;
class QTimer;

namespace mira_gui {

// The last lines of a source's setup log (`setup:<id>`) and a link to the whole of it, so a long
// install plainly hasn't stalled and a failed one says why. Hidden until Watch or ShowLast.
class SetupLog : public QWidget {
  Q_OBJECT

 public:
  SetupLog(const std::string& id, const QString& name, QWidget* parent);

  // Follows the log while `on`; off reads it once more, for how it ended.
  void Watch(bool on);
  // Shows how the last run ended, e.g. after a failure.
  void ShowLast();

 signals:
  // The newest "42% ..." line an installer wrote, 0..1.
  void Progress(double progress);

 private:
  void Poll();

  std::string id_;
  QLabel* tail_ = nullptr;
  QTimer* timer_ = nullptr;
  bool busy_ = false;
};

}  // namespace mira_gui
