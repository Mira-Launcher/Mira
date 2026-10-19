#include "AboutCard.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include "../dialogs/LogWindow.h"
#include "../game/CardParts.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/WindowCards.h"

#ifndef MIRA_VERSION
#define MIRA_VERSION "dev"
#endif

namespace mira_gui {
namespace {

constexpr int kLogoSize = 44;
constexpr char kRepository[] = "https://github.com/Mira-Launcher/Mira";

// Version, Qt and the system, as a bug report wants them.
QString VersionDetails() {
  return QString("Mira %1\nQt %2\n%3 (%4 %5)")
      .arg(MIRA_VERSION, qVersion(), QSysInfo::prettyProductName(), QSysInfo::kernelType(),
           QSysInfo::kernelVersion());
}

QToolButton* LinkButton(QWidget* parent, const QString& url) {
  auto* button = new QToolButton(parent);
  button->setAutoRaise(true);
  icons::Follow(button, icons::Glyph::External);
  button->setToolTip(url);
  QObject::connect(button, &QToolButton::clicked, parent,
                   [url] { QDesktopServices::openUrl(QUrl(url)); });
  return button;
}

QLabel* Below(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  return label;
}

}  // namespace

AboutCard::AboutCard(QWidget* parent) : SettingsCard(QString(), parent) {
  setFixedWidth(440);
  SetProminentTitle();
  logo_ = new QLabel(this);
  logo_->setFixedSize(kLogoSize, kLogoSize);
  ApplyLogo();
  SetLeading(logo_);
  AddCardHeading(
      this, "Mira", "Close", [this] { cards::Close(this); },
      QString("Version %1").arg(MIRA_VERSION));
  auto* copy = new QToolButton(this);
  copy->setAutoRaise(true);
  icons::Follow(copy, icons::Glyph::Copy);
  copy->setToolTip("Copy version details");
  // A tick for a moment says it worked, without a notice.
  connect(copy, &QToolButton::clicked, this, [copy] {
    QGuiApplication::clipboard()->setText(VersionDetails());
    copy->setIcon(icons::For(icons::Glyph::CheckCircle, theme::Current().success));
    QTimer::singleShot(1500, copy, [copy] { copy->setIcon(icons::For(icons::Glyph::Copy)); });
  });
  Header()->insertWidget(Header()->count() - 1, copy, 0, Qt::AlignTop);

  auto* source = new SettingRow("Source code", QString(), this);
  source->SetBelow(Below(source, QString(kRepository).mid(QString("https://").size())));
  source->AddControl(LinkButton(source, kRepository));
  AddRow(source);

  auto* report = new SettingRow("Report a problem", QString(), this);
  report->AddControl(LinkButton(report, QString(kRepository) + "/issues/new"));
  AddRow(report);

  auto* authors = new SettingRow("Made by", QString(), this);
  auto* names = Below(authors,
                      "<a href=\"https://github.com/AriGood\">AriGood</a> and "
                      "<a href=\"https://github.com/eMondri\">eMondri</a>");
  names->setOpenExternalLinks(true);
  authors->SetBelow(names);
  AddRow(authors);

  auto* license = new SettingRow("License", QString(), this);
  license->SetBelow(Below(license, "GPL-3.0-or-later"));
  license->AddControl(LinkButton(license, "https://www.gnu.org/licenses/gpl-3.0.html"));
  AddRow(license);

  auto* log = new SettingRow("Service log", QString(), this);
  auto* view = new QPushButton("View", log);
  view->setToolTip("What Mira's background service is doing, live");
  connect(view, &QPushButton::clicked, this,
          [this] { LogWindow::Open(this, "daemon", "Mira service"); });
  log->AddControl(view);
  AddRow(log);
}

void AboutCard::ApplyLogo() {
  // The largest icon there is, so it isn't stretched on a scaled screen.
  const qreal ratio = logo_->devicePixelRatioF();
  QPixmap pixmap = QPixmap(":/icons/256x256/apps/mira.png")
                       .scaled(QSize(kLogoSize, kLogoSize) * ratio, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
  pixmap.setDevicePixelRatio(ratio);
  logo_->setPixmap(pixmap);
}

}  // namespace mira_gui
