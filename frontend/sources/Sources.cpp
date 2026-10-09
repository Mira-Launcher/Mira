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
      {"lutris", "Lutris", Kind::Local, QColor("#f39c12")},
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
  }
  return {};
}

std::string SourceIdOf(const std::string& game_source) {
  // A store's own launcher is how its games run, not one of them.
  if (game_source == "launcher") return {};
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
