#include "LiveCard.h"

#include "../components/Labels.h"
#include "../components/SettingRow.h"

namespace mira_gui {

void LiveCard::AddLive(std::function<void()> fill) {
  fill();
  live_.push_back(std::move(fill));
}

void LiveCard::AddInfoRow(const QString& label, std::function<QString()> text,
                          std::function<QString()> tooltip) {
  auto* row = new SettingRow(label, QString(), this);
  auto* value = new ElidedLabel(QString(), row);
  SetTextStyle(value, text::kSubtle);
  value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  row->AddControl(value, 1);
  AddRow(row);
  AddLive([row, value, text = std::move(text), tooltip = std::move(tooltip)] {
    value->setText(text());
    if (tooltip) row->Label()->setToolTip(tooltip());
  });
}

void LiveCard::Refresh() {
  for (const auto& fill : live_) fill();
}

}  // namespace mira_gui
