#include "ModManagerPicker.h"

#include <QLineEdit>
#include <QPushButton>

#include "../components/Format.h"

namespace mira_gui {

ModManagerPicker::ModManagerPicker(ArtworkStore* artwork, const QString& manager_name,
                                   QWidget* parent)
    : GamePicker(artwork, "Back to " + manager_name, parent) {
  SetTitle("Add " + manager_name + " to games");
}

void ModManagerPicker::Open(const std::vector<const GameSummary*>& games) {
  Clear();
  std::vector<const GameSummary*> shown = games;
  std::vector<const GameSummary*> hidden;
  SortAndSplit(shown, hidden);
  if (!shown.empty()) AddSection(QString("Windows games · %1").arg(shown.size()), shown, {});
  if (!hidden.empty())
    AddSection(QString("Hidden · %1").arg(hidden.size()), hidden, {}, /*hidden=*/true);
  FinishSections();
  Search()->setFocus();
}

void ModManagerPicker::UpdateApply() {
  const int count = TickedCount();
  ApplyButton()->setText(QString("Add to %1").arg(Counted(count, "game")));
  ApplyButton()->setEnabled(count > 0);
}

void ModManagerPicker::Apply() {
  emit Picked(TickedIds());
  emit Closed();
}

}  // namespace mira_gui
