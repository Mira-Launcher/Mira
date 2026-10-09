#include "RunInPrefixDialog.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

#include "../app/Notify.h"
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

#include <QFileDialog>

#include "../client/api/Games.h"
#include "../widgets/PathField.h"
#include "../widgets/PopupDialog.h"

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
  form->addRow("Executable", PathRow(exe_, [this] {
    const QString selected = QFileDialog::getOpenFileName(
        this, "Select executable", QString::fromStdString(install_path_));
    return selected.isEmpty() ? selected : RelativeIfInside(selected, install_path_);
  }));

  args_ = new QLineEdit(this);
  args_->setPlaceholderText("Optional, e.g. /S for a silent install");
  form->addRow("Arguments", args_);
  layout->addLayout(form);

  run_ = AddDialogButtons(this, layout, "Run");
  // Nothing to run until a path is picked, so disabling this says so, instead
  // of a popup only reachable by clicking Run first to find out.
  run_->setEnabled(false);
  connect(run_, &QPushButton::clicked, this, &RunInPrefixDialog::Run);
  // Opens 50 px taller than its contents need.
  resize(sizeHint().width(), sizeHint().height() + 50);
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
