#include "UnclearDropCard.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace mira_gui {
namespace {

QString HomeRelative(const std::string& path) {
  const QString text = QString::fromStdString(path);
  const QString home = QDir::homePath();
  return text.startsWith(home + "/") ? "~" + text.mid(home.size()) : text;
}

}  // namespace

UnclearDropCard::UnclearDropCard(const UnclearDrop& drop, QWidget* parent)
    : SettingsCard(QString(), parent), path_(drop.path) {
  setFixedWidth(560);

  const QString name = drop.name.empty() ? QFileInfo(QString::fromStdString(drop.path)).fileName()
                                         : QString::fromStdString(drop.name);
  auto* title = new QLabel(QString("Where does %1 go?").arg(name), this);
  title->setProperty("role", "heading");
  title->setWordWrap(true);
  SetTitleWidget(title);

  AddRow(new SettingRow(HomeRelative(drop.path), QString::fromStdString(drop.reason), this));

  auto* buttons = new QWidget(this);
  auto* buttons_layout = new QHBoxLayout(buttons);
  buttons_layout->setContentsMargins(18, 12, 18, 10);
  buttons_layout->setSpacing(8);
  buttons_layout->addStretch(1);
  auto* later = new QPushButton("Ask again later", buttons);
  connect(later, &QPushButton::clicked, this, &UnclearDropCard::CloseRequested);
  buttons_layout->addWidget(later);
  for (const auto& [id, label] : drop.choices) {
    auto* choice = new QPushButton(QString::fromStdString(label), buttons);
    connect(choice, &QPushButton::clicked, this, [this, id = id] { emit Chosen(path_, id); });
    buttons_layout->addWidget(choice);
  }
  AddRow(buttons);
}

}  // namespace mira_gui
