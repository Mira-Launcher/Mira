#include "SourceSetupCard.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "../client/api/Stores.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "SourceText.h"

namespace mira_gui {

SourceSetupCard::SourceSetupCard(const SourceInfo& source, QWidget* parent)
    : SettingsCard("Set up " + source.name, parent), source_(source), id_(source.id.toStdString()) {
  setVisible(false);
  const SourceCopy copy = CopyFor(id_);

  QStringList titles;
  if (source_.kind == SourceInfo::Kind::Store)
    titles << "Get " + copy.tool << "Sign in to " + source_.name;
  if (IsLauncher()) titles << "Install " + source_.name;
  if (!titles.isEmpty() && !copy.import_button.isEmpty()) titles << "Import your " + copy.item + "s";
  for (int i = 0; i < titles.size(); ++i) {
    Step step;
    step.row = new QWidget(this);
    auto* column = new QVBoxLayout(step.row);
    column->setContentsMargins(18, 10, 18, 10);
    column->setSpacing(8);
    auto* line = new QHBoxLayout();
    line->setSpacing(12);
    step.marker = new QLabel(QString::number(i + 1), step.row);
    step.marker->setObjectName("step_marker");
    step.marker->setFixedSize(26, 26);
    step.marker->setAlignment(Qt::AlignCenter);
    line->addWidget(step.marker);
    step.title = new QLabel(titles[i], step.row);
    step.title->setObjectName("step_title");
    line->addWidget(step.title, /*stretch=*/1);
    column->addLayout(line);
    AddRow(step.row);
    steps_.push_back(step);
  }

  // Moved into whichever step is current (SetStep).
  body_ = new QWidget(this);
  auto* body = new QVBoxLayout(body_);
  body->setContentsMargins(38, 0, 0, 4);
  body->setSpacing(8);
  text_ = MakeLabel(body_, QString());
  body->addWidget(text_);

  button_ = new QPushButton(body_);
  button_->setIcon(icons::For(icons::Glyph::Download, theme::Current().on_accent));
  button_->setDefault(true);
  button_->setVisible(false);
  connect(button_, &QPushButton::clicked, this, &SourceSetupCard::StartSetup);
  auto* button_row = new QHBoxLayout();
  button_row->addWidget(button_);
  button_row->addStretch(1);
  body->addLayout(button_row);

  sign_in_row_ = new QWidget(body_);
  sign_in_row_->setVisible(false);
  auto* sign_in_layout = new QHBoxLayout(sign_in_row_);
  sign_in_layout->setContentsMargins(0, 0, 0, 0);
  open_login_ = new QPushButton("Open login page", sign_in_row_);
  connect(open_login_, &QPushButton::clicked, this, &SourceSetupCard::OpenLogin);
  credential_ = new QLineEdit(sign_in_row_);
  credential_->setPlaceholderText(copy.credential_placeholder);
  connect(credential_, &QLineEdit::returnPressed, this, &SourceSetupCard::SignIn);
  sign_in_ = new QPushButton("Sign in", sign_in_row_);
  sign_in_->setDefault(true);
  connect(sign_in_, &QPushButton::clicked, this, &SourceSetupCard::SignIn);
  sign_in_layout->addWidget(open_login_);
  sign_in_layout->addWidget(credential_, /*stretch=*/1);
  sign_in_layout->addWidget(sign_in_);
  body->addWidget(sign_in_row_);

  error_ = MakeLabel(body_, QString(), "error");
  error_->setVisible(false);
  body->addWidget(error_);
  body_->hide();
}

void SourceSetupCard::ShowStore(bool tool_installed, bool authenticated) {
  const SourceCopy copy = CopyFor(id_);
  error_->setVisible(false);
  // One step at a time: the tool, then the account.
  setVisible(!tool_installed || !authenticated);
  button_->setVisible(!tool_installed);
  button_->setEnabled(true);
  sign_in_row_->setVisible(tool_installed && !authenticated);
  SetStep(!tool_installed ? 0 : !authenticated ? 1 : 2);
  if (!tool_installed) {
    text_->setText("Mira uses " + copy.tool + " to talk to " + source_.name +
                   ". It's downloaded once, from its own releases.");
    button_->setText("Download " + copy.tool);
  } else if (!authenticated) {
    text_->setText(copy.sign_in_steps);
    open_login_->setVisible(true);
  }
}

void SourceSetupCard::ShowLauncher(const LauncherInfo& launcher, bool installing) {
  setVisible(!launcher.installed);
  SetStep(launcher.installed ? 1 : 0);
  if (launcher.installed) return;
  text_->setText(launcher.interactive_install
                     ? "Mira makes a Wine prefix for it and runs its installer. The installer's "
                       "window opens: click through it, then sign in."
                     : "Mira makes a Wine prefix for it and installs it there silently. Sign in "
                       "once it opens.");
  button_->setVisible(true);
  button_->setEnabled(!installing);
  button_->setText(installing ? "Installing…" : "Install " + source_.name);
  if (launcher.install_state == "failed" && !launcher.error.empty()) {
    mira_gui::ShowError(error_, "The last install failed.", launcher.error);
  }
}

void SourceSetupCard::ShowError(const QString& what, const ApiError& error) {
  setVisible(true);
  mira_gui::ShowError(error_, what, error);
}

void SourceSetupCard::ShowSetupFailed(const ApiError& error, bool tool_installed) {
  setVisible(true);
  if (tool_installed) text_->setText("Updating " + CopyFor(id_).tool + " failed.");
  button_->setEnabled(true);
  button_->setText(IsLauncher() ? "Install " + source_.name : "Retry download");
  mira_gui::ShowError(error_, "It failed.", error);
}

void SourceSetupCard::StartSetup() {
  button_->setEnabled(false);
  error_->setVisible(false);
  if (IsLauncher()) {
    button_->setText("Installing…");
    emit LauncherInstallStarted();
    api::InstallLauncherAsync(this, id_, [this](StoreActionResult result) {
      if (result.ok) return;  // the event finishes the job
      emit LauncherInstallFailed();
      button_->setEnabled(true);
      mira_gui::ShowError(error_, "Could not start it.", result.error);
    });
    return;
  }
  button_->setText("Downloading…");
  api::SetupStoreToolAsync(this, id_, [this](StoreActionResult result) {
    if (result.ok) {
      emit StatusChanged();
      return;
    }
    setVisible(true);
    button_->setEnabled(true);
    button_->setText("Retry download");
    mira_gui::ShowError(error_, "It failed.", result.error);
  });
}

void SourceSetupCard::SetStep(int current) {
  for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
    const Step& step = steps_[i];
    const char* state = i < current ? "done" : i == current ? "current" : "todo";
    step.marker->setText(i < current ? QString::fromUtf8("\xe2\x9c\x93") : QString::number(i + 1));
    step.marker->setProperty("state", state);
    step.marker->style()->unpolish(step.marker);
    step.marker->style()->polish(step.marker);
    step.title->setProperty("state", state);
    step.title->style()->unpolish(step.title);
    step.title->style()->polish(step.title);
    // In code: a stylesheet font-weight on a property state didn't apply here.
    QFont font = step.title->font();
    font.setWeight(i == current ? QFont::DemiBold : QFont::Normal);
    step.title->setFont(font);
  }
  if (current >= 0 && current < static_cast<int>(steps_.size())) {
    static_cast<QVBoxLayout*>(steps_[current].row->layout())->addWidget(body_);
    body_->show();
  }
}

void SourceSetupCard::OpenLogin() {
  open_login_->setEnabled(false);
  api::BeginStoreLoginAsync(this, id_, [this](LoginUrlResult result) {
    open_login_->setEnabled(true);
    if (!result.ok) {
      mira_gui::ShowError(error_, "Could not start the login.", result.error);
      return;
    }
    QDesktopServices::openUrl(QUrl(QString::fromStdString(result.url)));
  });
}

void SourceSetupCard::SignIn() {
  const QString pasted = credential_->text().trimmed();
  if (pasted.isEmpty()) return;
  sign_in_->setEnabled(false);
  sign_in_->setText("Signing in…");
  error_->setVisible(false);
  api::SignInStoreAsync(this, id_, pasted.toStdString(), [this](StoreActionResult result) {
    sign_in_->setEnabled(true);
    sign_in_->setText("Sign in");
    if (!result.ok) {
      mira_gui::ShowError(error_, "Could not sign in.", result.error);
      return;
    }
    credential_->clear();
    emit StatusChanged();
  });
}

}  // namespace mira_gui
