#pragma once

#include "../settings/SettingsCard.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace mira_gui {

// Asks for the hidden games' password before hidden mode shows them.
class HiddenUnlockCard : public SettingsCard {
  Q_OBJECT

public:
  explicit HiddenUnlockCard(QWidget* parent = nullptr);

signals:
  void Unlocked();
  void CloseRequested();

protected:
  void showEvent(QShowEvent* event) override;

private:
  void Unlock();

  QLineEdit* password_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* unlock_ = nullptr;
};

// Settings' row for the hidden games' password: sets, changes or removes it in a form that opens
// under the row.
class HiddenPasswordRow : public SettingRow {
  Q_OBJECT

public:
  explicit HiddenPasswordRow(QWidget* parent = nullptr);

private:
  enum class Form { None, Set, Change, Remove };
  void ShowState(bool password);
  void Open(Form form);
  void Submit();

  Form form_ = Form::None;
  QPushButton* set_ = nullptr;
  QPushButton* change_ = nullptr;
  QPushButton* remove_ = nullptr;
  QWidget* fields_ = nullptr;
  QLineEdit* current_ = nullptr;
  QLineEdit* next_ = nullptr;
  QLineEdit* repeat_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* submit_ = nullptr;
};

}  // namespace mira_gui
