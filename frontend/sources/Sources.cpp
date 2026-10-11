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
      {"duckstation", "DuckStation", Kind::Emulator, QColor("#fe6700"), "PlayStation"},
      {"pcsx2", "PCSX2", Kind::Emulator, QColor("#699cfe"), "PlayStation 2", true},
      {"ppsspp", "PPSSPP", Kind::Emulator, QColor("#4c9fb8"), "PSP"},
      {"rpcs3", "RPCS3", Kind::Emulator, QColor("#563cc8"), "PlayStation 3", true},
      {"shadps4", "shadPS4", Kind::Emulator, QColor("#3d57e2"), "PlayStation 4", true},
      {"xemu", "xemu", Kind::Emulator, QColor("#3c8f45"), "Xbox", true},
      {"dolphin", "Dolphin", Kind::Emulator, QColor("#00b5ff"), "GameCube and Wii"},
      {"cemu", "Cemu", Kind::Emulator, QColor("#00a2df"), "Wii U", true},
      {"eden", "Eden", Kind::Emulator, QColor("#b64ef8"), "Switch", true},
      {"ryujinx", "Ryujinx", Kind::Emulator, QColor("#fd8b62"), "Switch", true},
      {"melonds", "melonDS", Kind::Emulator, QColor("#fe544c"), "Nintendo DS"},
      {"azahar", "Azahar", Kind::Emulator, QColor("#b5a816"), "3DS"},
      {"mesen", "Mesen", Kind::Emulator, QColor("#933604"), "NES, SNES, Game Boy and PC Engine"},
      {"snes9x", "Snes9x", Kind::Emulator, QColor("#8683e1"), "SNES"},
      {"flycast", "Flycast", Kind::Emulator, QColor("#833de8"), "Dreamcast", true},
      {"bsnes", "bsnes", Kind::Emulator, QColor("#16a1ff"), "SNES", true},
      {"ares", "ares", Kind::Emulator, QColor("#c9b23a"), "NES, SNES, Nintendo 64, Genesis and Game Boy"},
      {"blastem", "BlastEm", Kind::Emulator, QColor("#3a3a3a"), "Genesis", true},
      {"mgba", "mGBA", Kind::Emulator, QColor("#4d299e"), "Game Boy and Game Boy Advance"},
      {"rmg", "RMG", Kind::Emulator, QColor("#02b3d3"), "Nintendo 64"},
      {"vita3k", "Vita3K", Kind::Emulator, QColor("#d4a900"), "PS Vita"},
      {"ymir", "Ymir", Kind::Emulator, QColor("#414db5"), "Saturn", true},
      {"mame", "MAME", Kind::Emulator, QColor("#4b9ecd"), "arcade"},
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

QString PlaysSearchText(const SourceInfo& source) {
  static const std::pair<const char*, const char*> kShort[] = {
      {"PlayStation 4", "ps4"}, {"PlayStation 3", "ps3"}, {"PlayStation 2", "ps2"}, {"PlayStation", "ps1 psx"},
      {"PS Vita", "vita psv"}, {"Nintendo 64", "n64"}, {"Nintendo DS", "nds"}, {"Game Boy Advance", "gba"},
      {"Game Boy", "gb gbc"}, {"GameCube", "gc ngc"}, {"Genesis", "mega drive megadrive md"},
      {"Dreamcast", "dc"}, {"Switch", "nx"}, {"PC Engine", "turbografx pce"}, {"arcade", "mame"}};
  QStringList words{source.plays};
  for (const auto& [name, short_names] : kShort) {
    if (source.plays.contains(name)) words << short_names;
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
