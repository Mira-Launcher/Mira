#include "SystemNotifier.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QObject>
#include <QStandardPaths>
#include <QStringList>
#include <QVariantMap>

#include <unordered_map>
#include <utility>

namespace mira_gui::notify::system_notifier {
namespace {

constexpr char kService[] = "org.freedesktop.Notifications";
constexpr char kPath[] = "/org/freedesktop/Notifications";

// Matches packaging/mira.desktop's basename, which lets the shell show Mira's own
// name and icon, and list it in per-application notification settings.
constexpr char kDesktopEntry[] = "mira";

QDBusInterface& Interface() {
  static QDBusInterface interface(kService, kPath, kService, QDBusConnection::sessionBus());
  return interface;
}

// Maps a sent notification's id to its action callback. The service reports
// a click as an ActionInvoked signal carrying only that id.
class ActionRouter : public QObject {
  Q_OBJECT

public:
  // Parented to the app, not a function-local static: it has to go before
  // the D-Bus connection does at shutdown.
  static ActionRouter& Instance() {
    static auto* router = new ActionRouter(QCoreApplication::instance());
    return *router;
  }

  void Track(uint id, std::function<void()> on_action) { pending_[id] = std::move(on_action); }

public slots:
  void OnActionInvoked(uint id, const QString& /*key*/) {
    const auto it = pending_.find(id);
    if (it == pending_.end()) return;
    std::function<void()> on_action = std::move(it->second);
    pending_.erase(it);
    if (on_action) on_action();
  }

  void OnClosed(uint id, uint /*reason*/) { pending_.erase(id); }

private:
  explicit ActionRouter(QObject* parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, "ActionInvoked", this,
                SLOT(OnActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kService, "NotificationClosed", this, SLOT(OnClosed(uint, uint)));
  }

  std::unordered_map<uint, std::function<void()>> pending_;
};

}  // namespace

bool DesktopEntryInstalled() {
  static const bool installed = [] {
    const QString file = QString(kDesktopEntry) + ".desktop";
    QStringList roots = {QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
    roots += QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString& root : roots) {
      if (QFileInfo::exists(root + "/applications/" + file)) return true;
    }
    return false;
  }();
  return installed;
}

bool Available() {
  static const bool available = Interface().isValid();
  return available;
}

bool Send(Urgency urgency, const QString& summary, const QString& body, const QString& action_label,
          std::function<void()> on_action) {
  if (!Available()) return false;

  const bool persistent = urgency == Urgency::Persistent;
  QVariantMap hints;
  // Spec urgency: 1 normal, 2 critical. KDE keeps a critical one on screen
  // until dismissed; a normal one goes to history after its timeout.
  hints["urgency"] = QVariant::fromValue(static_cast<uchar>(persistent ? 2 : 1));
  if (DesktopEntryInstalled()) hints["desktop-entry"] = QString(kDesktopEntry);

  // "default" is a click on the notification body; the second pair is the
  // visible button. Both run the same callback.
  QStringList actions;
  const bool has_action = !action_label.isEmpty() && on_action;
  if (has_action) actions = {"default", action_label, "open", action_label};

  // expire_timeout: 0 never expires, -1 is the desktop's own default.
  const QDBusReply<uint> reply =
      Interface().call("Notify", QString("Mira"), 0U, QString(kDesktopEntry), summary, body, actions,
                       hints, persistent ? 0 : -1);
  if (!reply.isValid()) return false;
  if (has_action) ActionRouter::Instance().Track(reply.value(), std::move(on_action));
  return true;
}

}  // namespace mira_gui::notify::system_notifier

#include "SystemNotifier.moc"
