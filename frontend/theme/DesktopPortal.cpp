#include "DesktopPortal.h"

#include <QColor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QMap>
#include <QObject>
#include <QVariantMap>

#include <optional>

#include "Theme.h"

namespace mira_gui::theme::portal {
namespace {

constexpr char kService[] = "org.freedesktop.portal.Desktop";
constexpr char kPath[] = "/org/freedesktop/portal/desktop";
constexpr char kInterface[] = "org.freedesktop.portal.Settings";
constexpr char kNamespace[] = "org.freedesktop.appearance";
// The first window waits on it, so a portal that's slow to start doesn't hold Mira up.
constexpr int kTimeoutMs = 250;

std::optional<QColor> g_accent;
std::optional<Qt::ColorScheme> g_scheme;

// A value as the portal sends it, which Read (unlike ReadOne) wraps in one more variant.
QVariant Unwrap(QVariant value) {
  while (value.userType() == qMetaTypeId<QDBusVariant>()) {
    value = qvariant_cast<QDBusVariant>(value).variant();
  }
  return value;
}

// (ddd) in 0..1; anything outside means the desktop has no accent set.
std::optional<QColor> AccentFrom(const QVariant& value) {
  if (value.userType() != qMetaTypeId<QDBusArgument>()) return std::nullopt;
  const QDBusArgument arg = qvariant_cast<QDBusArgument>(value);
  double r = -1, g = -1, b = -1;
  arg.beginStructure();
  arg >> r >> g >> b;
  arg.endStructure();
  for (const double part : {r, g, b}) {
    if (part < 0 || part > 1) return std::nullopt;
  }
  return QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
}

// 1 is dark, 2 light, 0 no preference.
std::optional<Qt::ColorScheme> SchemeFrom(const QVariant& value) {
  switch (value.toUInt()) {
    case 1:
      return Qt::ColorScheme::Dark;
    case 2:
      return Qt::ColorScheme::Light;
    default:
      return std::nullopt;
  }
}

void Take(const QString& key, const QVariant& raw) {
  const QVariant value = Unwrap(raw);
  if (key == "accent-color") g_accent = AccentFrom(value);
  if (key == "color-scheme") g_scheme = SchemeFrom(value);
}

class Listener : public QObject {
  Q_OBJECT

public slots:
  void SettingChanged(const QString& space, const QString& key, const QDBusVariant& value) {
    if (space != kNamespace) return;
    Take(key, value.variant());
    SetDesktopAppearance(g_accent, g_scheme);
  }
};

}  // namespace

void Watch() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) return;
  QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface, "ReadAll");
  call << QStringList{kNamespace};
  const QDBusMessage reply = bus.call(call, QDBus::Block, kTimeoutMs);
  if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
    QMap<QString, QVariantMap> all;
    qvariant_cast<QDBusArgument>(reply.arguments().first()) >> all;
    const QVariantMap appearance = all.value(kNamespace);
    for (auto it = appearance.begin(); it != appearance.end(); ++it) Take(it.key(), it.value());
    SetDesktopAppearance(g_accent, g_scheme);
  }
  static Listener* listener = new Listener;
  bus.connect(kService, kPath, kInterface, "SettingChanged", listener,
              SLOT(SettingChanged(QString, QString, QDBusVariant)));
}

}  // namespace mira_gui::theme::portal

#include "DesktopPortal.moc"
