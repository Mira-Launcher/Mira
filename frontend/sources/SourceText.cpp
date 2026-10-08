#include "SourceText.h"

#include <QLabel>
#include <QStyle>

#include "../app/ErrorHelp.h"

namespace mira_gui {

SourceCopy CopyFor(const std::string& id) {
  if (id == "steam") {
    return {"Your installed Steam games. Steam itself still installs and updates them.", "", "", "",
            "Scan Steam library"};
  }
  if (id == "epic") {
    return {"Epic Games Store, through Legendary. Games run through Mira's own Wine/Proton.",
            "Legendary",
            "Open Epic's login page and sign in, then paste the code it shows (or the whole "
            "page) here.",
            "authorizationCode, or the whole page", "Import installed games"};
  }
  if (id == "gog") {
    return {"GOG, through gogdl. Games install into your games folder.", "gogdl",
            "Open GOG's login page and sign in. It ends on a blank page: paste that page's "
            "address here.",
            "Address of the blank page, or its code", "Import installed games"};
  }
  if (id == "itch") {
    return {"itch.io, through butler.", "butler",
            "Create an API key on itch.io and paste it here. Keys don't expire.", "API key",
            "Import installed games"};
  }
  if (id == "amazon") {
    return {"Amazon Games, through nile.", "nile",
            "Open Amazon's login page and sign in. It ends on an amazon.com page: paste that "
            "page's address here.",
            "Address of the page login ends on", "Import installed games"};
  }
  if (id == "humble") {
    return {"Humble Bundle purchases, through humble-cli. Downloads land in your games folder.",
            "humble-cli",
            "Sign in to Humble Bundle in your browser, then copy the value of its "
            "_simpleauth_sess cookie (developer tools → Storage or Application → Cookies) and "
            "paste it here.",
            "_simpleauth_sess cookie value", ""};
  }
  if (id == "battlenet") {
    return {"Battle.net runs in its own Wine prefix. Games you install in it show up here.", "", "",
            "", "Import games"};
  }
  if (id == "ubisoft") {
    return {"Ubisoft Connect runs in its own Wine prefix. Games you install in it show up here.",
            "", "", "", "Import games"};
  }
  if (id == "office") {
    return {"Word, Excel, PowerPoint and the other Microsoft 365 apps, in their own Wine prefix. They "
            "show up under Apps. Sign in to your Microsoft account in any app; it needs a Microsoft "
            "365 subscription.",
            "", "", "", "Import apps", "app"};
  }
  if (id == "ea") {
    return {"The EA app runs in its own Wine prefix. Games you install in it show up here.", "", "",
            "", "Import games"};
  }
  return {"Lutris's Wine and native games. Nothing is moved; they stay playable in Lutris too.", "",
          "", "", "Import Lutris games"};
}

void ShowLine(QLabel* label, const QString& text, const char* role) {
  label->setProperty("role", role);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->setText(text);
  label->setVisible(!text.isEmpty());
}

void ShowError(QLabel* label, const QString& what, const ApiError& error) {
  QString text = error_help::Describe(error);
  if (!text.isEmpty()) text[0] = text[0].toUpper();
  ShowLine(label, (what + " " + text).trimmed(), "error");
}

QString ImportOutcome(int added, int updated) {
  if (added == 0 && updated == 0) return "No new games found.";
  if (added == 0) return QString("No new games; %1 updated.").arg(updated);
  return QString("Added %1 game%2.").arg(added).arg(added == 1 ? "" : "s");
}

QString CountedHeading(const QString& text, int count) {
  return count > 0 ? QString("%1  <span style='font-weight:400; opacity:0.6'>%2</span>")
                         .arg(text)
                         .arg(count)
                   : text;
}

}  // namespace mira_gui
