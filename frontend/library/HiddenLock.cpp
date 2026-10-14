#include "HiddenLock.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/api/Hidden.h"

namespace mira_gui {
namespace {

QLineEdit* PasswordField(QWidget* parent) {
  auto* field = new QLineEdit(parent);
  field->setEchoMode(QLineEdit::Password);
  return field;
}

QLabel* ErrorLabel(QWidget* parent) {
  auto* error = new QLabel(parent);
  error->setProperty("role", "error");
  error->setWordWrap(true);
  error->hide();
  return error;
}

QString Problem(const ApiError& error, const QString& wrong) {
  return error.code == "wrong_password" ? wrong : error_help::Describe(error);
}

}  // namespace

HiddenUnlockCard::HiddenUnlockCard(QWidget* parent) : SettingsCard("Unlock hidden games", parent) {
  setFixedWidth(440);
  auto* row = new SettingRow("Password", QString(), this);
  password_ = PasswordField(row);
  password_->setMinimumWidth(240);
  row->AddControl(password_, 1);
  AddRow(row);

  error_ = ErrorLabel(this);
  error_->setContentsMargins(18, 4, 18, 0);
  AddRow(error_);

  auto* buttons = new QWidget(this);
  auto* layout = new QHBoxLayout(buttons);
  layout->setContentsMargins(18, 12, 18, 10);
  layout->setSpacing(8);
  layout->addStretch(1);
  auto* cancel = new QPushButton("Cancel", buttons);
  connect(cancel, &QPushButton::clicked, this, &HiddenUnlockCard::CloseRequested);
  layout->addWidget(cancel);
  unlock_ = new QPushButton("Unlock", buttons);
  unlock_->setDefault(true);
  connect(unlock_, &QPushButton::clicked, this, &HiddenUnlockCard::Unlock);
  layout->addWidget(unlock_);
  AddRow(buttons);

  connect(password_, &QLineEdit::returnPressed, this, &HiddenUnlockCard::Unlock);
}

void HiddenUnlockCard::showEvent(QShowEvent* event) {
  SettingsCard::showEvent(event);
  // After the overlay has taken focus for itself.
  QTimer::singleShot(0, password_, [this] { password_->setFocus(); });
}

void HiddenUnlockCard::Unlock() {
  if (!unlock_->isEnabled()) return;
  unlock_->setEnabled(false);
  error_->hide();
  api::UnlockHiddenAsync(this, password_->text().toStdString(), [this](api::HiddenState state) {
    unlock_->setEnabled(true);
    if (state.ok) {
      emit Unlocked();
      return;
    }
    error_->setText(Problem(state.error, "That isn't the password."));
    error_->show();
    password_->selectAll();
    password_->setFocus();
  });
}

HiddenPasswordRow::HiddenPasswordRow(QWidget* parent)
    : SettingRow("Password",
                 "Ask for a password before hidden mode shows hidden games. Until it's given, Mira "
                 "leaves them out everywhere, the mira command too.",
                 parent) {
  set_ = new QPushButton("Set password…", this);
  change_ = new QPushButton("Change…", this);
  remove_ = new QPushButton("Remove…", this);
  for (QPushButton* button : {set_, change_, remove_}) {
    button->hide();  // until mirad says whether one is set
    AddControl(button);
  }
  connect(set_, &QPushButton::clicked, this, [this] { Open(Form::Set); });
  connect(change_, &QPushButton::clicked, this, [this] { Open(Form::Change); });
  connect(remove_, &QPushButton::clicked, this, [this] { Open(Form::Remove); });

  fields_ = new QWidget(this);
  auto* outer = new QVBoxLayout(fields_);
  outer->setContentsMargins(0, 8, 0, 0);
  auto* form = new QFormLayout;
  current_ = PasswordField(fields_);
  next_ = PasswordField(fields_);
  repeat_ = PasswordField(fields_);
  form->addRow("Current password", current_);
  form->addRow("New password", next_);
  form->addRow("Repeat it", repeat_);
  outer->addLayout(form);
  error_ = ErrorLabel(fields_);
  outer->addWidget(error_);
  auto* buttons = new QHBoxLayout;
  buttons->addStretch(1);
  auto* cancel = new QPushButton("Cancel", fields_);
  connect(cancel, &QPushButton::clicked, this, [this] { Open(Form::None); });
  buttons->addWidget(cancel);
  submit_ = new QPushButton(fields_);
  submit_->setDefault(true);
  connect(submit_, &QPushButton::clicked, this, &HiddenPasswordRow::Submit);
  buttons->addWidget(submit_);
  outer->addLayout(buttons);
  for (QLineEdit* field : {current_, next_, repeat_}) {
    connect(field, &QLineEdit::returnPressed, this, &HiddenPasswordRow::Submit);
  }
  fields_->hide();
  SetBelow(fields_);

  api::GetHiddenStateAsync(this, [this](api::HiddenState state) {
    if (!state.ok) return notify::FailedRequest(this, "Could not check the hidden-games password.", state.error);
    ShowState(state.password);
  });
}

void HiddenPasswordRow::ShowState(bool password) {
  set_->setVisible(!password);
  change_->setVisible(password);
  remove_->setVisible(password);
}

void HiddenPasswordRow::Open(Form form) {
  form_ = form;
  for (QLineEdit* field : {current_, next_, repeat_}) field->clear();
  error_->hide();
  if (form == Form::None) {
    fields_->hide();
    return;
  }
  auto* layout = static_cast<QFormLayout*>(fields_->layout()->itemAt(0)->layout());
  layout->setRowVisible(current_, form != Form::Set);
  layout->setRowVisible(next_, form != Form::Remove);
  layout->setRowVisible(repeat_, form != Form::Remove);
  submit_->setText(form == Form::Set      ? "Set password"
                   : form == Form::Change ? "Change password"
                                          : "Remove password");
  fields_->show();
  (form == Form::Set ? next_ : current_)->setFocus();
}

void HiddenPasswordRow::Submit() {
  if (form_ == Form::None || !submit_->isEnabled()) return;
  const bool keeps = form_ != Form::Remove;
  if (keeps && next_->text().isEmpty()) {
    error_->setText("Type a new password.");
    error_->show();
    return;
  }
  if (keeps && next_->text() != repeat_->text()) {
    error_->setText("The two new passwords differ.");
    error_->show();
    return;
  }
  submit_->setEnabled(false);
  error_->hide();
  const std::string next = keeps ? next_->text().toStdString() : std::string();
  api::SetHiddenPasswordAsync(
      this, current_->text().toStdString(), next, [this](api::HiddenState state) {
        submit_->setEnabled(true);
        if (!state.ok) {
          error_->setText(Problem(state.error, "That isn't the current password."));
          error_->show();
          return;
        }
        ShowState(state.password);
        Open(Form::None);
      });
}

}  // namespace mira_gui
