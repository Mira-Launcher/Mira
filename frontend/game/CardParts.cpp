#include "CardParts.h"

#include <QLabel>

#include "../client/Types.h"
#include "../components/SheetCard.h"
#include "../library/ArtworkStore.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);

}  // namespace

QLabel* AddGameCardHeading(SheetCard* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork) {
  auto* cover = new QLabel(card);
  cover->setFixedSize(kCover);
  const auto show_cover = [card, cover, artwork, game] {
    cover->setPixmap(artwork->Cover(game, kCover, card->devicePixelRatioF()));
  };
  show_cover();
  QObject::connect(artwork, &ArtworkStore::CoverChanged, cover,
                   [show_cover, id = QString::fromStdString(game.id)](const QString& changed) {
                     if (changed == id) show_cover();
                   });
  card->SetLeading(cover);
  return card->SetHeading(title);
}

QString InstallerFormatName(const std::string& format) {
  if (format == "inno") return "Inno Setup";
  if (format == "nsis") return "NSIS";
  if (format == "msi") return "Windows Installer";
  return {};
}

}  // namespace mira_gui
