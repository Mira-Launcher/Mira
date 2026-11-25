#include "SavesConflictCard.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../client/Types.h"
#include "../client/api/Games.h"
#include "../components/Button.h"
#include "../components/Format.h"
#include "../components/Labels.h"
#include "../components/LaidOutButton.h"
#include "../components/Metrics.h"
#include "../library/ArtworkStore.h"
#include "../sources/Sources.h"
#include "SavesText.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(48, 72);

}  // namespace

LaidOutButton* MakeSavesChoice(QWidget* parent, const QString& name, const QString& detail,
                               const QString& effect, QLabel** detail_label) {
  auto* button = new LaidOutButton(parent);
  button->setObjectName("choice_card");
  button->setCursor(Qt::PointingHandCursor);
  auto* inside = new QVBoxLayout(button);
  inside->setContentsMargins(14, 12, 14, 12);
  inside->setSpacing(6);
  inside->addWidget(MakeLabel(button, name, text::kSection));
  // A blank line holds its room while it's unknown, so the cards don't grow when it arrives.
  QLabel* holds = MakeLabel(button, detail.isEmpty() ? QString(" ") : detail, text::kDetail);
  inside->addWidget(holds);
  if (detail_label != nullptr) *detail_label = holds;
  inside->addWidget(MakeLabel(button, effect, text::kDetail));
  for (QWidget* child : button->findChildren<QWidget*>())
    child->setAttribute(Qt::WA_TransparentForMouseEvents);
  QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  button->setSizePolicy(policy);
  return button;
}

SavesConflictCard::SavesConflictCard(const GameSummary& game, ArtworkStore* artwork,
                                     QWidget* parent)
    : SheetCard(QString(), parent), id_(game.id) {
  setFixedWidth(620);

  auto* cover = new QLabel(this);
  cover->setFixedSize(kCover);
  const auto show_cover = [this, cover, artwork, game] {
    cover->setPixmap(artwork->Cover(game, kCover, devicePixelRatioF()));
  };
  show_cover();
  connect(artwork, &ArtworkStore::CoverChanged, cover,
          [show_cover, id = QString::fromStdString(game.id)](const QString& changed) {
            if (changed == id) show_cover();
          });
  SetLeading(cover);

  // The store's name from the game, so the line is whole before the saves arrive.
  const SourceInfo* store = FindSourceInfo(QString::fromStdString(game.source));
  auto* heading = new QWidget(this);
  auto* heading_layout = new QVBoxLayout(heading);
  heading_layout->setContentsMargins(0, 0, 0, 0);
  heading_layout->setSpacing(4);
  QLabel* title = MakeLabel(
      heading, QString("Which saves should %1 start with?").arg(QString::fromStdString(game.name)),
      text::kHeading);
  heading_layout->addWidget(title);
  const bool in_store = game.source == "gog" || game.source == "epic";
  QLabel* place = MakeLabel(
      heading,
      in_store ? QString("They changed here and in %1's cloud since they were last in sync.")
                     .arg(store != nullptr ? store->name : QString::fromStdString(game.source))
               : QString("They changed here and elsewhere since they were last in sync."),
      text::kSubtle);
  heading_layout->addWidget(place);
  SetTitleWidget(heading);

  SetClose("Cancel", [this] { emit Chosen({}); });

  auto* choices = new QWidget(this);
  auto* choices_layout = new QHBoxLayout(choices);
  choices_layout->setContentsMargins(kRowPaddingX, 8, kRowPaddingX, 4);
  choices_layout->setSpacing(12);
  const auto choice = [&](const QString& name, QLabel** detail, const QString& effect,
                          const std::string& answer) {
    LaidOutButton* button = MakeSavesChoice(choices, name, QString(), effect, detail);
    connect(button, &QPushButton::clicked, this, [this, answer] { emit Chosen(answer); });
    choices_layout->addWidget(button, 1);
    return button;
  };
  LaidOutButton* keep =
      choice("Keep this computer's", &local_, "Uploads them over the cloud's", "keep_local");
  LaidOutButton* theirs =
      choice("Use the cloud's", &cloud_, "This computer's are backed up first", "use_cloud");
  theirs->setFocus();
  AddRow(choices);

  QHBoxLayout* buttons = ButtonRow();
  auto* skip = new Button("Play without syncing", ButtonKind::Text, this);
  skip->setToolTip("Start with the saves here and leave the cloud's as they are");
  connect(skip, &QPushButton::clicked, this, [this] { emit Chosen("skip"); });
  buttons->insertWidget(0, skip);
  auto* cancel = new QPushButton("Cancel", this);
  connect(cancel, &QPushButton::clicked, this, [this] { emit Chosen({}); });
  buttons->addWidget(cancel);

  api::GetSavesAsync(this, id_, [this, place, keep, theirs](SavesResult result) {
    if (!result.ok || !result.cloud) return;
    const CloudSaves& cloud = *result.cloud;
    place->setText(QString("They changed here and %1 since they were last in sync.")
                       .arg(saves_text::Place(cloud)));
    theirs->findChild<QLabel*>()->setText(saves_text::UseTheirs(cloud));
    keep->findChildren<QLabel*>().last()->setText(saves_text::KeepEffect(cloud));
    const QDateTime now = QDateTime::currentDateTime();
    if (cloud.local_at > 0) {
      local_->setText("Saved " + saves_text::When(cloud.local_at, now, true) + " · " +
                      saves_text::Files(cloud.files, cloud.bytes));
    }
    if (cloud.cloud_at > 0)
      cloud_->setText("Uploaded " + saves_text::When(cloud.cloud_at, now, true));
  });
}

}  // namespace mira_gui
