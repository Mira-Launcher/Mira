#pragma once

#include <QWidget>
#include <string>

class QLabel;
class QLineEdit;
class QPushButton;

namespace mira_gui {

class LinkDownloads;

// Asks for the password of the downloads waiting on one, one at a time, under the top bar's notice.
class PasswordPopup : public QWidget {
  Q_OBJECT

 public:
  PasswordPopup(LinkDownloads* downloads, QWidget* parent);
  // Starts with `id`, or the first one waiting.
  void ShowBelow(QWidget* anchor, const std::string& id = {});

 private:
  void Refresh();
  void Submit();

  LinkDownloads* downloads_;
  std::string current_;
  std::string shown_;  // whose password the field holds
  QLabel* name_ = nullptr;
  QLabel* count_ = nullptr;
  QLabel* error_ = nullptr;
  QLineEdit* password_ = nullptr;
  QPushButton* extract_ = nullptr;
};

}  // namespace mira_gui
