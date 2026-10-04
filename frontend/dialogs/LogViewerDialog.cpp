#include "LogViewerDialog.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include <utility>

#include "../client/MiradClient.h"
#include "../ui/ErrorHelp.h"

namespace mira_gui {

namespace {
constexpr int kLines = 200;
}

LogViewerDialog::LogViewerDialog(std::string game_id, QString game_name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)) {
  setWindowTitle(QString("Log: %1").arg(game_name));
  resize(720, 520);

  auto* layout = new QVBoxLayout(this);

  text_ = new QPlainTextEdit(this);
  text_->setReadOnly(true);
  text_->setLineWrapMode(QPlainTextEdit::NoWrap);
  QFont font("monospace");
  font.setStyleHint(QFont::Monospace);
  text_->setFont(font);
  layout->addWidget(text_, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* refresh = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);
  connect(refresh, &QPushButton::clicked, this, &LogViewerDialog::Refresh);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  Refresh();
}

void LogViewerDialog::Refresh() {
  MiradClient::GetGameLogAsync(this, game_id_, kLines, [this](GameLogResult result) {
    if (!result.ok) {
      // Said where the log would be, not as a notification over it.
      text_->setPlainText("Could not read this game's log: " + error_help::Describe(result.error));
      return;
    }
    if (result.lines.empty()) {
      text_->setPlainText("No log yet for this game.");
      return;
    }

    QString joined;
    for (const std::string& line : result.lines) {
      joined += QString::fromStdString(line);
      joined += '\n';
    }
    text_->setPlainText(joined);
    text_->moveCursor(QTextCursor::End);
  });
}

}  // namespace mira_gui
