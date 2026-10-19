#include "CardParts.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "../client/Types.h"
#include "../library/ArtworkStore.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);

}  // namespace

QLabel* AddCardHeading(SettingsCard* card, const QString& title, const QString& close_tip,
                       const std::function<void()>& close, const QString& subtitle) {
  auto* heading = new QLabel(title, card);
  heading->setProperty("role", "heading");
  heading->setWordWrap(true);
  if (subtitle.isEmpty()) {
    card->SetTitleWidget(heading);
  } else {
    auto* lines = new QWidget(card);
    auto* layout = new QVBoxLayout(lines);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    layout->addWidget(heading);
    auto* sub = new QLabel(subtitle, lines);
    sub->setProperty("role", "muted");
    sub->setWordWrap(true);
    layout->addWidget(sub);
    heading->setParent(lines);
    card->SetTitleWidget(lines);
  }

  auto* button = new QToolButton(card);
  button->setAutoRaise(true);
  icons::Follow(button, icons::Glyph::Close);
  button->setToolTip(close_tip);
  QObject::connect(button, &QToolButton::clicked, card, close);
  card->Header()->addWidget(button, 0, Qt::AlignTop);
  return heading;
}

QLabel* AddGameCardHeading(SettingsCard* card, const GameSummary& game, const QString& title,
                           ArtworkStore* artwork, const QString& close_tip,
                           const std::function<void()>& close) {
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
  return AddCardHeading(card, title, close_tip, close);
}

QToolButton* AddCardBack(SettingsCard* card, const std::function<void()>& back) {
  auto* button = new QToolButton(card);
  button->setAutoRaise(true);
  icons::Follow(button, icons::Glyph::ArrowLeft);
  button->setToolTip("Back");
  QObject::connect(button, &QToolButton::clicked, card, back);
  card->SetLeading(button);
  return button;
}

QLabel* AddErrorLine(SettingsCard* card) {
  auto* error = new QLabel(card);
  error->setProperty("role", "error");
  error->setWordWrap(true);
  error->setContentsMargins(18, 4, 18, 0);
  error->hide();
  card->AddFooterRow(error);
  return error;
}

QHBoxLayout* AddButtonRow(SettingsCard* card) {
  auto* buttons = new QWidget(card);
  auto* layout = new QHBoxLayout(buttons);
  layout->setContentsMargins(18, 12, 18, 10);
  layout->setSpacing(8);
  layout->addStretch(1);
  card->AddFooterRow(buttons);
  return layout;
}

QString InstallerFormatName(const std::string& format) {
  if (format == "inno") return "Inno Setup";
  if (format == "nsis") return "NSIS";
  if (format == "msi") return "Windows Installer";
  return {};
}

}  // namespace mira_gui
