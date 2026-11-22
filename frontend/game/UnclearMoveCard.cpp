#include "UnclearMoveCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "../components/Format.h"
#include "../components/SettingRow.h"
#include "../components/Text.h"
#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(32, 48);

}  // namespace

UnclearMoveCard::UnclearMoveCard(const UnclearMove& move, const GameLibraryModel* library,
                                 ArtworkStore* artwork, QWidget* parent)
    : SheetCard(QString(), parent), folder_(move.folder) {
  setFixedWidth(560);

  SetHeading(QString("Which game is %1?").arg(HomeRelative(QString::fromStdString(move.folder))));

  for (const auto& [id, name] : move.games) {
    auto* row = new SettingRow(QString::fromStdString(name), QString(), this);
    if (const GameSummary* game = library->Find(id)) {
      auto* cover = new QLabel(row);
      cover->setFixedSize(kCover);
      const auto show_cover = [this, cover, artwork, game = *game] {
        cover->setPixmap(artwork->Cover(game, kCover, devicePixelRatioF()));
      };
      show_cover();
      connect(artwork, &ArtworkStore::CoverChanged, cover,
              [show_cover, id = QString::fromStdString(game->id)](const QString& changed) {
                if (changed == id) show_cover();
              });
      row->AddAfterLabel(cover);
    }
    auto* pick = new QPushButton("This game", row);
    connect(pick, &QPushButton::clicked, this,
            [this, folder = move.folder, id = id] { emit Chosen(folder, id); });
    row->AddControl(pick);
    AddRow(row);
  }

  QHBoxLayout* buttons = ButtonRow();
  auto* later = new QPushButton("Ask again later", this);
  connect(later, &QPushButton::clicked, this, &UnclearMoveCard::CloseRequested);
  buttons->addWidget(later);
  auto* fresh = new QPushButton("It's a new game", this);
  connect(fresh, &QPushButton::clicked, this,
          [this, folder = move.folder] { emit Chosen(folder, std::string()); });
  buttons->addWidget(fresh);
}

}  // namespace mira_gui
