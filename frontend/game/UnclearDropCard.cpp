#include "UnclearDropCard.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "../components/Format.h"
#include "../components/SettingRow.h"
#include "../components/Text.h"

namespace mira_gui {

UnclearDropCard::UnclearDropCard(const UnclearDrop& drop, QWidget* parent)
    : SheetCard(QString(), parent), path_(drop.path) {
  setFixedWidth(560);

  const QString name = drop.name.empty() ? QFileInfo(QString::fromStdString(drop.path)).fileName()
                                         : QString::fromStdString(drop.name);
  SetHeading(QString("Where does %1 go?").arg(name));

  AddRow(new SettingRow(HomeRelative(QString::fromStdString(drop.path)),
                        QString::fromStdString(drop.reason), this));

  QHBoxLayout* buttons = ButtonRow();
  auto* later = new QPushButton("Ask again later", this);
  connect(later, &QPushButton::clicked, this, &UnclearDropCard::CloseRequested);
  buttons->addWidget(later);
  for (const auto& [id, label] : drop.choices) {
    auto* choice = new QPushButton(QString::fromStdString(label), this);
    connect(choice, &QPushButton::clicked, this, [this, id = id] { emit Chosen(path_, id); });
    buttons->addWidget(choice);
  }
}

}  // namespace mira_gui
