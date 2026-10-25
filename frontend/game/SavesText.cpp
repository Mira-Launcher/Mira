#include "SavesText.h"

#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

namespace mira_gui::saves_text {

QString When(std::int64_t at, const QDateTime& now, bool in_sentence) {
  const QDateTime when = QDateTime::fromSecsSinceEpoch(at, now.timeZone());
  const QString time = when.toString("HH:mm");
  const qint64 days = when.date().daysTo(now.date());
  if (days == 0) return (in_sentence ? "today, " : "Today, ") + time;
  if (days == 1) return (in_sentence ? "yesterday, " : "Yesterday, ") + time;
  const QString format = when.date().year() == now.date().year() ? "d MMM" : "d MMM yyyy";
  return QLocale().toString(when.date(), format) + ", " + time;
}

QString Reason(const std::string& reason) {
  if (reason == "after_play") return "After playing";
  if (reason == "first") return "Before the first launch";
  if (reason == "manual") return "Backed up by hand";
  if (reason == "before_restore") return "Before restoring";
  return QString::fromStdString(reason);
}

QString Summary(const SavesResult& saves, const QDateTime& now) {
  if (saves.last_error) return "The last backup failed";
  if (saves.status == "kept" && !saves.snapshots.empty()) {
    const SaveSnapshot& last = saves.snapshots.front();
    const QString count = saves.snapshots.size() == 1
                              ? QString("1 backup")
                              : QString("%1 backups").arg(saves.snapshots.size());
    if (last.reason != "after_play") return count + " · last " + When(last.taken_at, now, true);
    const QDate day = QDateTime::fromSecsSinceEpoch(last.taken_at, now.timeZone()).date();
    const qint64 days = day.daysTo(now.date());
    const QString session = days == 0   ? QString("today's session")
                            : days == 1 ? QString("yesterday's session")
                                        : "the session on " + QLocale().toString(day, "d MMM");
    return count + " · last after " + session;
  }
  if (saves.status == "ready") return "Saves found, not backed up yet";
  if (saves.status == "no_files") return "No saves yet";
  if (saves.status == "registry_only") return "Saves kept in the registry";
  if (saves.status == "steam_cloud") return "Saves kept by Steam Cloud";
  if (saves.status == "off") return "Saves not backed up";
  return "Save locations not known";
}

bool NeedsAttention(const SavesResult& saves) {
  return saves.last_error.has_value();
}

QString Path(const std::string& path, const QString& home) {
  const QString text = QString::fromStdString(path);
  static const QRegularExpression user(R"(/drive_c/users/[^/]+/)");
  if (const QRegularExpressionMatch match = user.match(text); match.hasMatch())
    return "prefix › " + text.mid(match.capturedEnd());
  if (!home.isEmpty() && (text == home || text.startsWith(home + '/')))
    return "~" + text.mid(home.size());
  return text;
}

QString StatusNote(const SavesResult& saves) {
  if (saves.status == "no_files") return "Nothing is saved in these folders yet.";
  if (saves.status == "registry_only")
    return "This game keeps its saves in the Windows registry, which Mira doesn't back up: "
           "restoring it would also undo every other setting in the prefix.";
  if (saves.status == "steam_cloud")
    return "Steam starts this game and keeps its saves in Steam Cloud, so Mira leaves them to it.";
  if (saves.status == "off") return "Backing up this game's saves is off.";
  if (saves.status == "no_entry")
    return "Mira doesn't know where this game keeps its saves. Add its save folders as more save "
           "locations.";
  return {};
}

}  // namespace mira_gui::saves_text
