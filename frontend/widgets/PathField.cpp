#include "PathField.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>

#include <system_error>

namespace mira_gui {

QString RelativeIfInside(const QString& selected, const std::filesystem::path& base) {
  std::error_code ec;
  const std::filesystem::path relative =
      std::filesystem::relative(selected.toStdString(), base, ec);
  const bool inside = !ec && !relative.empty() && *relative.begin() != "..";
  return inside ? QString::fromStdString(relative.string()) : selected;
}

QWidget* PathRow(QLineEdit* edit, std::function<QString()> pick) {
  auto* row = new QWidget(edit->parentWidget());
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  layout->addWidget(edit, /*stretch=*/1);
  auto* browse = new QPushButton("Browse…", row);
  layout->addWidget(browse);
  QObject::connect(browse, &QPushButton::clicked, row, [edit, pick = std::move(pick)] {
    const QString chosen = pick();
    if (chosen.isEmpty()) return;
    edit->setText(chosen);
    edit->setCursorPosition(0);
  });
  return row;
}

}  // namespace mira_gui
