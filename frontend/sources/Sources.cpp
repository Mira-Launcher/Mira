#include "Sources.h"

#include <QLabel>

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
      {"office", "Microsoft 365", Kind::Launcher, QColor("#d83b01")},
      {"adobe", "Adobe Creative Cloud", Kind::Launcher, QColor("#da1f26")},
      {"lutris", "Lutris", Kind::Local, QColor("#f39c12")},
      {"duckstation", "DuckStation", Kind::Emulator, QColor("#c99a1e"), "PlayStation"},
      {"pcsx2", "PCSX2", Kind::Emulator, QColor("#2b6cb0"), "PlayStation 2"},
      {"ppsspp", "PPSSPP", Kind::Emulator, QColor("#2f8fcf"), "PSP"},
      {"rpcs3", "RPCS3", Kind::Emulator, QColor("#3d3d99"), "PlayStation 3"},
      {"shadps4", "shadPS4", Kind::Emulator, QColor("#5865f2"), "PlayStation 4"},
      {"xemu", "xemu", Kind::Emulator, QColor("#5a9e2f"), "Xbox"},
      {"dolphin", "Dolphin", Kind::Emulator, QColor("#1f9bd8"), "GameCube and Wii"},
      {"cemu", "Cemu", Kind::Emulator, QColor("#2f6fd1"), "Wii U"},
      {"eden", "Eden", Kind::Emulator, QColor("#7b3fe4"), "Switch"},
      {"ryujinx", "Ryujinx", Kind::Emulator, QColor("#4a90c8"), "Switch"},
      {"melonds", "melonDS", Kind::Emulator, QColor("#c4413f"), "Nintendo DS"},
      {"azahar", "Azahar", Kind::Emulator, QColor("#e0582b"), "3DS"},
      {"mesen", "Mesen", Kind::Emulator, QColor("#3f7a3f"), "NES, SNES, Game Boy and PC Engine"},
      {"snes9x", "Snes9x", Kind::Emulator, QColor("#8a4fbf"), "SNES"},
      {"flycast", "Flycast", Kind::Emulator, QColor("#e07b1f"), "Dreamcast"},
      {"bsnes", "bsnes", Kind::Emulator, QColor("#b03a48"), "SNES"},
      {"ares", "ares", Kind::Emulator, QColor("#2e7d6b"), "NES, SNES, Nintendo 64, Genesis and Game Boy"},
      {"blastem", "BlastEm", Kind::Emulator, QColor("#3a3a3a"), "Genesis"},
      {"mgba", "mGBA", Kind::Emulator, QColor("#6a3d9a"), "Game Boy and Game Boy Advance"},
      {"rmg", "RMG", Kind::Emulator, QColor("#b5432a"), "Nintendo 64"},
      {"vita3k", "Vita3K", Kind::Emulator, QColor("#1f4e9c"), "PS Vita"},
      {"ymir", "Ymir", Kind::Emulator, QColor("#4b5d73"), "Saturn"},
      {"mame", "MAME", Kind::Emulator, QColor("#8a6d1e"), "arcade"},
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

QLabel* MakeSourceBadge(const SourceInfo& source, int size, QWidget* parent, bool dim) {
  auto* badge = new QLabel(source.name.left(1), parent);
  badge->setFixedSize(size, size);
  badge->setAlignment(Qt::AlignCenter);
  SetSourceBadgeDim(badge, source, dim);
  return badge;
}

void SetSourceBadgeDim(QLabel* badge, const SourceInfo& source, bool dim) {
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

}  // namespace mira_gui
