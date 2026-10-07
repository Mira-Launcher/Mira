#include "UnclearMoveCard.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(32, 48);

QString HomeRelative(const std::string& path) {
  const QString text = QString::fromStdString(path);
  const QString home = QDir::homePath();
  return text.startsWith(home + "/") ? "~" + text.mid(home.size()) : text;
}

}  // namespace

UnclearMoveCard::UnclearMoveCard(const UnclearMove& move, const GameLibraryModel* library,
                                 ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), folder_(move.folder) {
  setFixedWidth(560);

  auto* title = new QLabel(QString("Which game is %1?").arg(HomeRelative(move.folder)), this);
  title->setProperty("role", "heading");
  title->setWordWrap(true);
  SetTitleWidget(title);

  for (const auto& [id, name] : move.games) {
    auto* row = new SettingRow(QString::fromStdString(name), QString(), this);
    if (const GameSummary* game = library->Find(id)) {
      auto* cover = new QLabel(row);
      cover->setFixedSize(kCover);
      cover->setPixmap(artwork->Cover(*game, kCover, devicePixelRatioF()));
      row->AddAfterLabel(cover);
    }
    auto* pick = new QPushButton("This game", row);
    connect(pick, &QPushButton::clicked, this, [this, folder = move.folder, id = id] { emit Chosen(folder, id); });
    row->AddControl(pick);
    AddRow(row);
  }

  auto* buttons = new QWidget(this);
  auto* buttons_layout = new QHBoxLayout(buttons);
  buttons_layout->setContentsMargins(18, 12, 18, 10);
  buttons_layout->setSpacing(8);
  buttons_layout->addStretch(1);
  auto* later = new QPushButton("Ask again later", buttons);
  connect(later, &QPushButton::clicked, this, &UnclearMoveCard::CloseRequested);
  buttons_layout->addWidget(later);
  auto* fresh = new QPushButton("It's a new game", buttons);
  connect(fresh, &QPushButton::clicked, this, [this, folder = move.folder] { emit Chosen(folder, std::string()); });
  buttons_layout->addWidget(fresh);
  AddRow(buttons);
}

}  // namespace mira_gui
