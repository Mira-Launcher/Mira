#pragma once

#include <QColor>
#include <QString>

#include <vector>

class QLabel;
class QWidget;

namespace mira_gui {

struct SourceInfo {
  enum class Kind {
    Store,     // a helper tool, an account, and what the account owns
    Launcher,  // a Windows launcher installed into its own prefix
    Local,     // reads what another program already installed
  };
  QString id;    // mirad's own name: "steam", "epic", ...; also the games' `source`
  QString name;  // shown in the sidebar and as the page title
  Kind kind = Kind::Local;
  QColor color;  // the source's icon tile
};

const std::vector<SourceInfo>& AllSources();

// nullptr for a source id Mira doesn't list (a scanned or manual game).
const SourceInfo* FindSourceInfo(const QString& id);

// "Store", "Launcher" or "Local", for the small tag beside a source's name.
QString KindLabel(SourceInfo::Kind kind);

// The source's initial on its color, `size` pixels square; faded when `dim`.
QLabel* MakeSourceBadge(const SourceInfo& source, int size, QWidget* parent, bool dim = false);
// Fades a badge from MakeSourceBadge, e.g. while its source is off or not set up.
void SetSourceBadgeDim(QLabel* badge, const SourceInfo& source, bool dim);
// The kind tag itself.
QLabel* MakeKindTag(const SourceInfo& source, QWidget* parent);

}  // namespace mira_gui
