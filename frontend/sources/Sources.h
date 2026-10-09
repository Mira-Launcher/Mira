#pragma once

#include <QColor>
#include <QString>

#include <string>
#include <vector>

class QLabel;
class QWidget;

namespace mira_gui {

struct SourceInfo {
  enum class Kind {
    Store,     // a helper tool, an account, and what the account owns
    Launcher,  // a Windows launcher installed into its own prefix
    Local,     // games already on this computer: another program's, or from no source at all
  };
  QString id;    // mirad's own name: "steam", "epic", ...; also the games' `source`
  QString name;  // shown in the sidebar and as the page title
  Kind kind = Kind::Local;
  QColor color;  // the source's icon tile
};

const std::vector<SourceInfo>& AllSources();

// nullptr for a source id Mira doesn't list (a scanned or manual game).
const SourceInfo* FindSourceInfo(const QString& id);

// "Store", "Launcher" or "On this computer", for the small tag beside a source's name.
QString KindLabel(SourceInfo::Kind kind);

// The listed source a game with this `source` belongs to: its own when listed, "local" for games
// from no source (scanned, added by hand, desktop entries), empty for a store's own launcher.
std::string SourceIdOf(const std::string& game_source);

// Every source, in the saved order where it names them. One it doesn't name (added since it was
// saved) goes right after its neighbour in AllSources' order, so Local, listed first, leads.
std::vector<QString> OrderSources(const std::vector<QString>& saved);

// The source's initial on its color, `size` pixels square; faded when `dim`.
QLabel* MakeSourceBadge(const SourceInfo& source, int size, QWidget* parent, bool dim = false);
// Fades a badge from MakeSourceBadge, e.g. while its source is off or not set up.
void SetSourceBadgeDim(QLabel* badge, const SourceInfo& source, bool dim);
// The kind tag itself.
QLabel* MakeKindTag(const SourceInfo& source, QWidget* parent);

}  // namespace mira_gui
