#include "RemoveModManagerCard.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QStringList>

#include "../components/Button.h"
#include "../components/Format.h"
#include "../components/Labels.h"
#include "../components/SettingRow.h"
#include "../components/WindowCards.h"
#include "../sources/Sources.h"

namespace mira_gui {
namespace {

constexpr int kBadge = 40;

}  // namespace

RemoveModManagerCard::RemoveModManagerCard(const QString& game_name, const SourceInfo& manager,
                                           QWidget* parent)
    : LiveCard(QString(), parent), manager_name_(manager.name), vortex_(manager.id == "vortex") {
  setFixedWidth(520);
  SetLeading(MakeSourceBadge(manager, kBadge, this));
  SetTitle("Remove " + manager_name_ + " from " + game_name);
  SetProminentTitle();

  const auto known = [this] { return summary_.has_value() && summary_->ok; };
  const auto unknown = [this] { return summary_.has_value() ? "Couldn't check" : "…"; };
  if (vortex_) {
    AddInfoRow("Mod files in the game's folder", [this, known, unknown] {
      if (!known()) return QString(unknown());
      return summary_->deployed == 0 ? QString("None")
                                     : QString("%1 taken out").arg(summary_->deployed);
    });
    AddInfoRow("Game files it set aside", [this, known, unknown] {
      if (!known()) return QString(unknown());
      return summary_->restored == 0 ? QString("None")
                                     : QString("%1 put back").arg(summary_->restored);
    });
  }
  AddInfoRow(
      vortex_ ? "Its mods, downloads and settings" : "Its instance: mods, downloads and profiles",
      [this, known, unknown] {
        if (!known()) return QString(unknown());
        if (summary_->deleted.empty()) return QString("None found");
        return SizeText(static_cast<qint64>(summary_->bytes)) + " · deleted";
      },
      [this, known] {
        if (!known()) return QString();
        QStringList folders;
        for (const std::string& folder : summary_->deleted)
          folders << HomeRelative(QString::fromStdString(folder));
        return folders.join('\n');
      });

  QHBoxLayout* buttons = ButtonRow();
  // Why Remove is off, in the same place whichever reason, so nothing moves when it appears.
  note_ = MakeLabel(this, QString(), text::kSubtle, /*wrap=*/false);
  buttons->insertWidget(0, note_, 1);
  auto* cancel = AddCancel([this] { cards::Close(this); });
  confirm_ = new Button("Remove", ButtonKind::Danger, this);
  confirm_->setEnabled(false);
  connect(confirm_, &QPushButton::clicked, this, &RemoveModManagerCard::Confirmed);
  buttons->addWidget(confirm_);
  cancel->setFocus();
}

void RemoveModManagerCard::SetSummary(const ModManagerPurgeResult& summary) {
  summary_ = summary;
  Refresh();
  confirm_->setEnabled(summary.ok && !summary.open);
  note_->setText(!summary.ok    ? QString("Couldn't check what it would delete")
                 : summary.open ? "Close " + manager_name_ + " first"
                                : QString());
}

}  // namespace mira_gui
