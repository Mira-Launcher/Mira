#include "SourceSetupCard.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../client/api/Config.h"
#include "../client/api/Logs.h"
#include "../client/api/Stores.h"
#include "../dialogs/LogWindow.h"
#include "../system/PackageInstall.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"
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
  icons::Follow(button_, icons::Glyph::Download, &theme::Tokens::on_accent);
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

  packages_row_ = new QWidget(body_);
  packages_row_->setVisible(false);
  auto* packages_layout = new QHBoxLayout(packages_row_);
  packages_layout->setContentsMargins(0, 0, 0, 0);
  packages_text_ = MakeLabel(packages_row_, QString(), "muted");
  packages_layout->addWidget(packages_text_, /*stretch=*/1);
  auto* install_packages = new QPushButton("Install packages", packages_row_);
  connect(install_packages, &QPushButton::clicked, this, [this] {
    system::EnsurePackages(this, packages_, source_.name, [this](bool) { CheckPackages(); });
  });
  packages_layout->addWidget(install_packages);
  body->addWidget(packages_row_);

  error_ = MakeLabel(body_, QString(), "error");
  error_->setVisible(false);
  body->addWidget(error_);

  // What setup is doing, so it's plain it hasn't stalled.
  log_box_ = new QWidget(body_);
  log_box_->setVisible(false);
  auto* log_layout = new QVBoxLayout(log_box_);
  log_layout->setContentsMargins(0, 0, 0, 0);
  log_layout->setSpacing(4);
  progress_ = new ProgressRail(log_box_);
  progress_->setVisible(false);
  log_layout->addWidget(progress_);
  log_tail_ = new QLabel(log_box_);
  log_tail_->setObjectName("setup_log_tail");
  log_tail_->setWordWrap(true);
  log_tail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont mono("monospace");
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSizeF(mono.pointSizeF() * 0.9);
  log_tail_->setFont(mono);
  log_tail_->setProperty("role", "muted");
  log_layout->addWidget(log_tail_);
  auto* open_log = new QPushButton("View full log", log_box_);
  open_log->setObjectName("text_button");
  connect(open_log, &QPushButton::clicked, this,
          [this] { LogWindow::Open(this, "setup:" + source_.id, source_.name + " setup"); });
  auto* log_row = new QHBoxLayout();
  log_row->addWidget(open_log);
  log_row->addStretch(1);
  log_layout->addLayout(log_row);
  body->addWidget(log_box_);
  log_timer_ = new QTimer(this);
  connect(log_timer_, &QTimer::timeout, this, &SourceSetupCard::PollLog);
  body_->hide();
}

void SourceSetupCard::ShowStore(bool tool_installed, bool authenticated) {
  const SourceCopy copy = CopyFor(id_);
  error_->setVisible(false);
  if (tool_installed) log_timer_->stop();
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
  packages_ = launcher.packages;
  CheckPackages();
  WatchLog(installing);
  if (!installing && launcher.install_state == "failed") log_box_->setVisible(true);
  if (launcher.install_state == "failed" && !launcher.error.empty()) {
    mira_gui::ShowError(error_, "The last install failed.", launcher.error);
  }
}

void SourceSetupCard::ShowError(const QString& what, const ApiError& error) {
  setVisible(true);
  mira_gui::ShowError(error_, what, error);
}

void SourceSetupCard::ShowSetupFailed(const ApiError& error, bool tool_installed) {
  WatchLog(false);
  log_box_->setVisible(true);
  setVisible(true);
  if (tool_installed) text_->setText("Updating " + CopyFor(id_).tool + " failed.");
  button_->setEnabled(true);
  button_->setText(IsLauncher() ? "Install " + source_.name : "Retry download");
  mira_gui::ShowError(error_, "It failed.", error);
}

void SourceSetupCard::InstallLauncher() {
  button_->setText("Installing…");
  emit LauncherInstallStarted();
  api::InstallLauncherAsync(this, id_, [this](StoreActionResult result) {
    if (result.ok) return;  // the event finishes the job
    emit LauncherInstallFailed();
    button_->setEnabled(true);
    mira_gui::ShowError(error_, "Could not start it.", result.error);
  });
}

void SourceSetupCard::CheckPackages() {
  if (packages_.empty()) return packages_row_->setVisible(false);
  api::GetSystemPackagesAsync(this, packages_, [this](SystemPackagesResult result) {
    packages_row_->setVisible(result.ok && !result.missing.empty());
    if (!packages_row_->isVisible()) return;
    QStringList names;
    for (const std::string& name : result.missing) names << QString::fromStdString(name);
    packages_text_->setText("Needs " + names.join(", ") + " from your system. Installing " + source_.name +
                            " installs it first.");
  });
}

void SourceSetupCard::WatchLog(bool on) {
  if (on) {
    progress_->setVisible(false);
    log_box_->setVisible(true);
    if (!log_timer_->isActive()) log_timer_->start(1500);
    PollLog();
  } else {
    log_timer_->stop();
    if (log_box_->isVisible()) PollLog();  // the last lines, with how it ended
  }
}

void SourceSetupCard::PollLog() {
  if (log_busy_) return;
  log_busy_ = true;
  api::GetLogAsync(this, "setup:" + id_, std::nullopt, 6, [this](LogResult result) {
    log_busy_ = false;
    if (!result.ok) return;
    QStringList lines;
    for (const std::string& line : result.lines) lines << QString::fromStdString(line);
    if (!result.live.empty()) lines << QString::fromStdString(result.live);
    while (lines.size() > 6) lines.removeFirst();
    // The newest "42% of ..." line the installer reported, as the rail above the log.
    static const QRegularExpression percent_line(R"(^\s*(\d+(?:\.\d+)?)%)");
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
      const QRegularExpressionMatch match = percent_line.match(*line);
      if (!match.hasMatch()) continue;
      progress_->SetProgress(match.captured(1).toDouble() / 100);
      progress_->setVisible(true);
      break;
    }
    log_tail_->setTextFormat(Qt::PlainText);
    log_tail_->setText(lines.isEmpty() ? QString("Waiting for output…") : lines.join('\n'));
  });
}

void SourceSetupCard::StartSetup() {
  button_->setEnabled(false);
  error_->setVisible(false);
  WatchLog(true);
  if (IsLauncher()) {
    if (packages_.empty()) return InstallLauncher();
    button_->setText("Checking packages…");
    system::EnsurePackages(this, packages_, source_.name, [this](bool ready) {
      CheckPackages();
      if (ready) return InstallLauncher();
      WatchLog(false);
      button_->setEnabled(true);
      button_->setText("Install " + source_.name);
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
