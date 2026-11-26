#include "CardParts.h"

#include <QLabel>

#include "../client/Types.h"
#include "../components/SheetCard.h"
#include "../library/ArtworkStore.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);

}  // namespace

QLabel* MakeCover(QWidget* parent, const GameSummary& game, QSize size, ArtworkStore* artwork) {
  auto* cover = new QLabel(parent);
  cover->setFixedSize(size);
  const auto show = [cover, artwork, game, size] {
    cover->setPixmap(artwork->Cover(game, size, cover->devicePixelRatioF()));
  };
  show();
  QObject::connect(artwork, &ArtworkStore::CoverChanged, cover,
                   [show, id = QString::fromStdString(game.id)](const QString& changed) {
                     if (changed == id) show();
                   });
  return cover;
}

QLabel* AddGameCardHeading(SheetCard* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork) {
  card->SetLeading(MakeCover(card, game, kCover, artwork));
  return card->SetHeading(title);
}

QString InstallerFormatName(const std::string& format) {
  if (format == "inno") return "Inno Setup";
  if (format == "nsis") return "NSIS";
  if (format == "msi") return "Windows Installer";
  return {};
}

}  // namespace mira_gui
