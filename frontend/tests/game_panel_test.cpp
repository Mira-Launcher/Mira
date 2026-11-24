#include <doctest.h>

#include <QLabel>
#include <QLayout>
#include <QPushButton>

#include "NavWindow.h"
#include "game/GamePanel.h"

using namespace mira_gui;

TEST_CASE("The game panel's saves line shows all its text in the narrow and the wide panel") {
  nav_test::App();
  for (const int width : {268, 340}) {
    for (const QString& text :
         QStringList{"12 backups · last after the session on 14 Nov", "3 backups · last today",
                     "Saves changed here and in Steam's cloud"}) {
      CAPTURE(width);
      CAPTURE(text.toStdString());
      GamePanel panel(nullptr);
      panel.SetSavesShown(true);
      panel.SetSaves(text, false);
      panel.resize(width, 900);
      panel.show();
      panel.layout()->activate();
      QPushButton* line = nullptr;  // the saves line is the panel line holding labels
      for (auto* button : panel.findChildren<QPushButton*>("panel_line"))
        if (button->layout() != nullptr) line = button;
      REQUIRE(line != nullptr);
      CHECK(line->geometry().right() < panel.width());
      QString shown;
      for (QLabel* part : line->findChildren<QLabel*>()) {
        if (!part->isVisible() || part->text() == "·") continue;
        shown += shown.isEmpty() ? part->text() : " · " + part->text();
        const QRect needs = part->fontMetrics().boundingRect(QRect(0, 0, part->width(), 10000),
                                                             Qt::TextWordWrap, part->text());
        CHECK(needs.width() <= part->width());
        CHECK(needs.height() <= part->height());
        CHECK(line->rect().contains(part->geometry()));
      }
      CHECK(shown == text);
    }
  }
}
