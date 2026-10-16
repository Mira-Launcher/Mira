#include "AboutPanel.h"

#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

#include "../theme/Theme.h"
#include "../widgets/Labels.h"

#ifndef MIRA_VERSION
#define MIRA_VERSION "dev"
#endif

namespace mira_gui {
namespace {

constexpr int kLogoSize = 88;

QLabel* Line(QWidget* parent, const QString& text, const char* role = nullptr) {
  QLabel* label = MakeLabel(parent, text, role);
  label->setAlignment(Qt::AlignCenter);
  label->setOpenExternalLinks(true);
  return label;
}

}  // namespace

AboutPanel::AboutPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 24, 16, 16);
  layout->setSpacing(8);

  layout->addStretch(1);

  logo_ = new QLabel(this);
  logo_->setAlignment(Qt::AlignCenter);
  ApplyLogo();
  layout->addWidget(logo_);

  layout->addWidget(Line(this, "Mira", "heading"));
  layout->addWidget(Line(this, QString("Version %1").arg(MIRA_VERSION), "muted"));
  layout->addSpacing(8);
  layout->addWidget(
      Line(this,
           "One library for native games and Windows games run through Wine or Proton, "
           "with Steam and Lutris folded in alongside them."));
  layout->addSpacing(8);
  layout->addWidget(Line(this,
                         "By <a href=\"https://github.com/AriGood\">AriGood</a> and "
                         "<a href=\"https://github.com/eMondri\">eMondri</a><br>"
                         "<a href=\"https://github.com/Mira-Launcher/Mira\">"
                         "github.com/Mira-Launcher/Mira</a>"
                         "<br><a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
                         "GPL-3.0-or-later</a>",
                         "muted"));

  layout->addStretch(2);

  layout->addWidget(Line(this, "Select a game to see its details.", "muted"));
}

void AboutPanel::ApplyLogo() {
  // The largest icon there is, so it isn't stretched on a scaled screen.
  const qreal ratio = logo_->devicePixelRatioF();
  QPixmap pixmap = QPixmap(":/icons/256x256/apps/mira.png")
                       .scaled(QSize(kLogoSize, kLogoSize) * ratio, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
  pixmap.setDevicePixelRatio(ratio);
  logo_->setPixmap(pixmap);
}

}  // namespace mira_gui
