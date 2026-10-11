#include "Sources.h"

#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace mira_gui {

const std::vector<SourceInfo>& AllSources() {
  using Kind = SourceInfo::Kind;
  static const std::vector<SourceInfo> sources = {
      {"local", "Local", Kind::Local, QColor("#5f6b7a")},
      {"steam", "Steam", Kind::Local, QColor("#2a475e")},
      {"epic", "Epic Games", Kind::Store, QColor("#4a4a4a")},
      {"gog", "GOG", Kind::Store, QColor("#86328a")},
      {"itch", "itch.io", Kind::Store, QColor("#fa5c5c")},
      {"amazon", "Amazon Games", Kind::Store, QColor("#ff9900")},
      {"humble", "Humble Bundle", Kind::Store, QColor("#cc2929")},
      {"battlenet", "Battle.net", Kind::Launcher, QColor("#148eff")},
      {"ubisoft", "Ubisoft Connect", Kind::Launcher, QColor("#0070ff")},
      {"ea", "EA app", Kind::Launcher, QColor("#ff4747")},
      {"office", "Microsoft 365", Kind::Launcher, QColor("#d83b01"), {}, true},
      {"adobe", "Adobe Creative Cloud", Kind::Launcher, QColor("#da1f26"), {}, true},
      {"lutris", "Lutris", Kind::Local, QColor("#f39c12")},
      {"duckstation", "DuckStation", Kind::Emulator, QColor("#fe6700"), {"ps1"}},
      {"pcsx2", "PCSX2", Kind::Emulator, QColor("#699cfe"), {"ps2"}, true},
      {"ppsspp", "PPSSPP", Kind::Emulator, QColor("#4c9fb8"), {"psp"}},
      {"rpcs3", "RPCS3", Kind::Emulator, QColor("#563cc8"), {"ps3"}, true},
      {"shadps4", "shadPS4", Kind::Emulator, QColor("#3d57e2"), {"ps4"}, true},
      {"xemu", "xemu", Kind::Emulator, QColor("#3c8f45"), {"xbox"}, true},
      {"xenia", "Xenia Canary", Kind::Emulator, QColor("#d8a73a"), {"xbox360"}, true},
      {"dolphin", "Dolphin", Kind::Emulator, QColor("#00b5ff"), {"gamecube", "wii"}},
      {"cemu", "Cemu", Kind::Emulator, QColor("#00a2df"), {"wiiu"}},
      {"eden", "Eden", Kind::Emulator, QColor("#b64ef8"), {"switch"}, true},
      {"ryujinx", "Ryujinx", Kind::Emulator, QColor("#fd8b62"), {"switch"}, true},
      {"melonds", "melonDS", Kind::Emulator, QColor("#fe544c"), {"nds"}},
      {"azahar", "Azahar", Kind::Emulator, QColor("#b5a816"), {"3ds"}},
      {"mesen", "Mesen", Kind::Emulator, QColor("#933604"), {"nes", "snes", "gb", "gbc", "pce", "sms", "gg", "ws"}},
      {"snes9x", "Snes9x", Kind::Emulator, QColor("#8683e1"), {"snes"}},
      {"flycast", "Flycast", Kind::Emulator, QColor("#833de8"), {"dreamcast"}},
      {"bsnes", "bsnes", Kind::Emulator, QColor("#16a1ff"), {"snes"}},
      {"ares", "ares", Kind::Emulator, QColor("#c9b23a"),
       {"nes", "snes", "n64", "genesis", "32x", "sms", "gg", "sg1000", "gb", "gbc", "gba", "pce", "ngp", "ws",
        "colecovision", "atari2600"}},
      {"blastem", "BlastEm", Kind::Emulator, QColor("#3a3a3a"), {"genesis"}},
      {"mgba", "mGBA", Kind::Emulator, QColor("#4d299e"), {"gb", "gbc", "gba"}},
      {"rmg", "RMG", Kind::Emulator, QColor("#02b3d3"), {"n64"}},
      {"vita3k", "Vita3K", Kind::Emulator, QColor("#d4a900"), {"psvita"}},
      {"ymir", "Ymir", Kind::Emulator, QColor("#414db5"), {"saturn"}, true},
      {"mednafen", "Mednafen", Kind::Emulator, QColor("#7fb81c"), {"saturn", "ps1", "pce", "vb", "ngp", "ws", "lynx"}},
      {"stella", "Stella", Kind::Emulator, QColor("#c8892e"), {"atari2600"}},
      {"kyty", "KytyPS5", Kind::Emulator, QColor("#5b8cff"), {"ps5"}, true},
      {"mupen64plus", "Mupen64Plus", Kind::Emulator, QColor("#d32f2f"), {"n64"}},
      {"redream", "Redream", Kind::Emulator, QColor("#2f7bd8"), {"dreamcast"}},
      {"atari800", "atari800", Kind::Emulator, QColor("#8f9299"), {"atari8bit", "atari5200"}},
      {"hatari", "Hatari", Kind::Emulator, QColor("#3c9a2b"), {"atarist"}},
      {"fsuae", "FS-UAE", Kind::Emulator, QColor("#7b2d8e"), {"amiga"}},
      {"vice", "VICE", Kind::Emulator, QColor("#5a5a5a"), {"c64"}},
      {"openmsx", "openMSX", Kind::Emulator, QColor("#d9a21b"), {"msx"}},
      {"uzdoom", "UZDoom", Kind::Emulator, QColor("#2e8b9a"), {"doom"}},
      {"easyrpg", "EasyRPG Player", Kind::Emulator, QColor("#6aa84f"), {"rpgmaker"}},
      {"mame", "MAME", Kind::Emulator, QColor("#4b9ecd"), {"arcade"}},
      {"dosbox", "DOSBox Staging", Kind::Emulator, QColor("#3e7bd6"), {"dos"}},
      {"scummvm", "ScummVM", Kind::Emulator, QColor("#2fb84a"), {"adventure"}},
  };
  return sources;
}

const SourceInfo* FindSourceInfo(const QString& id) {
  const std::vector<SourceInfo>& sources = AllSources();
  const auto it = std::ranges::find(sources, id, &SourceInfo::id);
  return it == sources.end() ? nullptr : &*it;
}

QString KindLabel(SourceInfo::Kind kind) {
  switch (kind) {
    case SourceInfo::Kind::Store:
      return "Store";
    case SourceInfo::Kind::Launcher:
      return "Launcher";
    case SourceInfo::Kind::Local:
      return "On this computer";
    case SourceInfo::Kind::Emulator:
      return "Emulator";
  }
  return {};
}

std::string SourceIdOf(const GameSummary& game) {
  const std::string& game_source = game.source;
  // A store's own launcher is how its games run, not one of them, unless it's an app (Creative Cloud).
  if (game_source == "launcher") return IsApp(game) ? game.source_ref : std::string();
  if (game_source != "local" && FindSourceInfo(QString::fromStdString(game_source)) != nullptr) return game_source;
  return "local";
}

std::vector<QString> OrderSources(const std::vector<QString>& saved) {
  std::vector<QString> order;
  for (const QString& id : saved) {
    if (FindSourceInfo(id) != nullptr && std::ranges::find(order, id) == order.end()) order.push_back(id);
  }
  const std::vector<SourceInfo>& all = AllSources();
  for (auto source = all.begin(); source != all.end(); ++source) {
    if (std::ranges::find(order, source->id) != order.end()) continue;
    auto at = order.begin();
    for (auto before = source; before != all.begin();) {
      const auto it = std::ranges::find(order, (--before)->id);
      if (it != order.end()) {
        at = it + 1;
        break;
      }
    }
    order.insert(at, source->id);
  }
  return order;
}

namespace {

// What each console id in the daemon's emulator table is called, and the other names people search it by.
struct Console {
  const char* id;
  const char* name;  // empty: shown under the one before it (Game Boy Color under Game Boy)
  const char* also;
};
constexpr Console kConsoles[] = {
    {"ps1", "PlayStation", "psx"}, {"ps2", "PlayStation 2", ""}, {"ps3", "PlayStation 3", ""},
    {"ps4", "PlayStation 4", ""}, {"ps5", "PlayStation 5", ""}, {"psp", "PSP", ""}, {"psvita", "PS Vita", "vita psv"},
    {"xbox", "Xbox", ""}, {"xbox360", "Xbox 360", "x360"}, {"gamecube", "GameCube", "gc ngc"}, {"wii", "Wii", ""}, {"wiiu", "Wii U", ""},
    {"switch", "Switch", "nx"}, {"nes", "NES", "famicom"}, {"snes", "SNES", "super famicom"},
    {"n64", "Nintendo 64", ""}, {"gb", "Game Boy", ""}, {"gbc", "", "game boy color"},
    {"gba", "Game Boy Advance", ""}, {"nds", "Nintendo DS", ""}, {"3ds", "3DS", ""},
    {"genesis", "Genesis", "mega drive megadrive md"}, {"32x", "32X", ""}, {"sms", "Master System", ""},
    {"gg", "Game Gear", ""}, {"sg1000", "SG-1000", ""},
    {"saturn", "Saturn", ""}, {"dreamcast", "Dreamcast", "dc"}, {"pce", "PC Engine", "turbografx"},
    {"ngp", "Neo Geo Pocket", "ngpc"}, {"ws", "WonderSwan", "wsc"}, {"vb", "Virtual Boy", ""},
    {"lynx", "Lynx", "atari lynx"}, {"atari2600", "Atari 2600", "2600 vcs"}, {"colecovision", "ColecoVision", ""},
    {"atari8bit", "Atari 8-bit", "atari 800 xl xe"}, {"atari5200", "Atari 5200", ""}, {"atarist", "Atari ST", ""},
    {"amiga", "Amiga", ""}, {"c64", "Commodore 64", "c64 commodore"}, {"msx", "MSX", ""},
    {"doom", "Doom-engine", "doom heretic hexen wad"}, {"rpgmaker", "RPG Maker", "rpg maker 2000 2003"},
    {"arcade", "arcade", ""}, {"dos", "DOS", "ms-dos pc"}, {"adventure", "adventure", "point and click scumm lucasarts sierra"},
};

const Console* FindConsole(const QString& id) {
  for (const Console& console : kConsoles) {
    if (id == console.id) return &console;
  }
  return nullptr;
}

}  // namespace

QString PlaysText(const SourceInfo& source) {
  QStringList names;
  for (const QString& id : source.consoles) {
    const Console* console = FindConsole(id);
    if (console == nullptr) names << id;
    else if (*console->name != '\0') names << console->name;
  }
  if (names.size() < 2) return names.join(QString());
  const QString last = names.takeLast();
  return names.join(", ") + " and " + last;
}

QString PlaysSearchText(const SourceInfo& source) {
  QStringList words{PlaysText(source)};
  for (const QString& id : source.consoles) {
    words << id;
    if (const Console* console = FindConsole(id)) words << console->also;
  }
  return words.join(' ').toLower();
}

QPixmap SourceLogo(const SourceInfo& source, int size, qreal dpr) {
  const QPixmap logo(":/logos/" + source.id + ".png");
  if (logo.isNull()) return {};
  QPixmap scaled = logo.scaled(QSize(size, size) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  scaled.setDevicePixelRatio(dpr);
  return scaled;
}

QLabel* MakeSourceBadge(const SourceInfo& source, int size, QWidget* parent, bool dim) {
  auto* badge = new QLabel(source.name.left(1), parent);
  badge->setFixedSize(size, size);
  badge->setAlignment(Qt::AlignCenter);
  SetSourceBadgeDim(badge, source, dim);
  return badge;
}

void SetSourceBadgeDim(QLabel* badge, const SourceInfo& source, bool dim) {
  if (QPixmap logo = SourceLogo(source, badge->width(), badge->devicePixelRatioF()); !logo.isNull()) {
    if (dim) {
      QPixmap faded(logo.size());
      faded.setDevicePixelRatio(logo.devicePixelRatio());
      faded.fill(Qt::transparent);
      QPainter painter(&faded);
      painter.setOpacity(0.45);
      painter.drawPixmap(0, 0, logo);
      painter.end();
      logo = faded;
    }
    badge->setText({});
    badge->setStyleSheet("background: transparent;");
    badge->setPixmap(logo);
    return;
  }
  QColor fill = source.color;
  if (dim) fill.setAlphaF(0.55);
  badge->setStyleSheet(QString("background: %1; color: white; border-radius: %2px; font-weight: 700;")
                           .arg(fill.name(QColor::HexArgb))
                           .arg(badge->width() / 4));
}

QLabel* MakeKindTag(const SourceInfo& source, QWidget* parent) {
  auto* tag = new QLabel(KindLabel(source.kind), parent);
  tag->setObjectName("kind_tag");
  return tag;
}

QLabel* MakeBetaTag(QWidget* parent) {
  auto* tag = new QLabel("Beta", parent);
  tag->setObjectName("beta_tag");
  tag->setToolTip("Works, but not everything does yet");
  return tag;
}

}  // namespace mira_gui
