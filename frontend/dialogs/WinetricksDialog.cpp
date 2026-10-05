#include "WinetricksDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

#include "../client/MiradClient.h"
#include "../client/EventHub.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../theme/Theme.h"

namespace mira_gui {

namespace {

// No catalog endpoint exists, just a shortlist of common verbs, alongside
// the free-text field.
constexpr const char* kCommonVerbs[] = {
    "corefonts", "vcrun2019", "vcrun2017", "dotnet48", "dotnet6",
    "d3dx9",     "d3dx11_43", "xact",      "physx",    "gdiplus",
};

}  // namespace

WinetricksDialog::WinetricksDialog(std::string game_id, QString game_name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)) {
  setWindowTitle(QString("Winetricks: %1").arg(game_name));
  setMinimumWidth(480);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* explanation = new QLabel(
      QString("Runs a winetricks verb inside \"%1\"'s own Wine/Proton prefix.").arg(game_name),
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  verb_ = new QComboBox(this);
  verb_->setEditable(true);
  verb_->setInsertPolicy(QComboBox::NoInsert);
  verb_->lineEdit()->setPlaceholderText("Verb, e.g. corefonts");
  for (const char* verb : kCommonVerbs) verb_->addItem(verb);
  verb_->setCurrentIndex(-1);
  connect(verb_->lineEdit(), &QLineEdit::textChanged, this,
          [this](const QString& text) { run_->setEnabled(!text.trimmed().isEmpty()); });
  layout->addWidget(verb_);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  status_->setProperty("role", "muted");
  layout->addWidget(status_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  run_ = buttons->addButton("Run", QDialogButtonBox::ActionRole);
  run_->setEnabled(false);
  connect(run_, &QPushButton::clicked, this, &WinetricksDialog::Run);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
}

void WinetricksDialog::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  mira_gui::theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void WinetricksDialog::Run() {
  const std::string verb = verb_->currentText().trimmed().toStdString();
  if (verb.empty()) return;  // Run is disabled for this case

  verb_->setEnabled(false);
  run_->setEnabled(false);
  SetStatus(QString("Starting %1…").arg(QString::fromStdString(verb)));

  MiradClient::RunWinetricksAsync(this, game_id_, verb, [this, verb](TricksResult result) {
    if (!result.ok) {
      verb_->setEnabled(true);
      run_->setEnabled(true);
      SetStatus(QString("Could not start %1: %2")
                    .arg(QString::fromStdString(verb), error_help::Describe(result.error)),
                /*error=*/true);
      return;
    }
    SetStatus(QString("Running %1…").arg(QString::fromStdString(verb)));
  });
}

void WinetricksDialog::HandleEvent(const std::string& type, const std::string& data) {
  TricksEvent event;
  if (!MiradClient::ParseTricksEvent(type, data, &event)) return;
  if (event.id != game_id_) return;

  const QString verb = QString::fromStdString(event.verb);
  if (event.state == "started") {
    SetStatus(QString("Running %1…").arg(verb));
    return;
  }

  verb_->setEnabled(true);
  run_->setEnabled(!verb_->currentText().trimmed().isEmpty());
  if (event.state == "failed") {
    SetStatus(QString("%1 failed: %2").arg(verb, error_help::Describe(event.error)),
              /*error=*/true);
    return;
  }
  SetStatus(QString("%1 finished.").arg(verb));
}

}  // namespace mira_gui
