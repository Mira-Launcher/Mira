#include "RunInPrefixDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

#include "../app/Notify.h"
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>
#include <utility>

#include "../client/api/Games.h"

namespace mira_gui {

RunInPrefixDialog::RunInPrefixDialog(std::string game_id, const std::string& install_path,
                                     const QString& name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)), install_path_(install_path) {
  setWindowTitle("Run in prefix");
  // 680, not the old 520, for a real install path next to the Browse
  // button. setMinimumWidth, not resize(680, 0): resize() marks the widget
  // explicitly sized, clamping it to the layout's bare minimum instead of
  // its sizeHint() on first show, which is what clipped the button's text.
  setMinimumWidth(680);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* explanation = new QLabel(
      QString("Runs an executable inside \"%1\"'s own Wine/Proton prefix. If the game has no "
              "prefix yet, one is created first. This is how you run an installer for a game "
              "that needs installing.")
          .arg(name),
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* form = new QFormLayout();
  exe_ = new QLineEdit(this);
  exe_->setPlaceholderText("Path, absolute or relative to the install folder");
  connect(exe_, &QLineEdit::textChanged, this,
          [this](const QString& text) { run_->setEnabled(!text.trimmed().isEmpty()); });

  auto* browse = new QPushButton("Browse…", this);
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString start_dir =
        install_path_.empty() ? QString() : QString::fromStdString(install_path_);
    const QString selected = QFileDialog::getOpenFileName(this, "Select executable", start_dir);
    if (selected.isEmpty()) return;

    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(selected.toStdString(), install_path_, ec);
    exe_->setText(!ec && !relative.empty() ? QString::fromStdString(relative.string()) : selected);
    exe_->setCursorPosition(0);
  });

  // A QWidget wrapper, not a bare QHBoxLayout passed to addRow(): QFormLayout
  // sized the row to the line edit's own height, not the taller button's,
  // clipping the button's text.
  auto* exe_row_widget = new QWidget(this);
  auto* exe_row = new QHBoxLayout(exe_row_widget);
  exe_row->setContentsMargins(0, 0, 0, 0);
  exe_row->setSpacing(8);
  exe_row->addWidget(exe_, /*stretch=*/1);
  exe_row->addWidget(browse);
  form->addRow("Executable", exe_row_widget);

  args_ = new QLineEdit(this);
  args_->setPlaceholderText("Optional, e.g. /S for a silent install");
  form->addRow("Arguments", args_);
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  run_ = buttons->addButton("Run", QDialogButtonBox::AcceptRole);
  // Nothing to run until a path is picked, so disabling this says so, instead
  // of a popup only reachable by clicking Run first to find out.
  run_->setEnabled(false);
  connect(run_, &QPushButton::clicked, this, &RunInPrefixDialog::Run);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void RunInPrefixDialog::Run() {
  const std::string exe = exe_->text().trimmed().toStdString();
  if (exe.empty()) return;  // the Run button is disabled for this case

  // Provisioning a prefix on demand is genuinely slow (it's initialising
  // Wine/Proton), and the request doesn't return until the process exists,
  // so the dialog says what it's doing rather than appearing frozen.
  setEnabled(false);
  run_->setText("Starting…");
  api::RunInPrefixAsync(this, game_id_, exe, args_->text().toStdString(),
                                [this](RunInPrefixResult result) {
                                  setEnabled(true);
                                  run_->setText("Run");
                                  if (!result.ok) {
                                    mira_gui::notify::FailedRequest(this, "Could not run that.", result.error);
                                    return;
                                  }
                                  accept();
                                });
}

}  // namespace mira_gui
