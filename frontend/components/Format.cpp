#include "Format.h"

#include <QDir>
#include <QLocale>

namespace mira_gui {

QString JoinNames(const std::vector<std::string>& names) {
  QStringList list;
  for (const std::string& name : names) list << QString::fromStdString(name);
  return list.join(", ");
}

QString SizeText(qint64 bytes) {
  return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString HomeRelative(const QString& path) {
  const QString home = QDir::homePath();
  return path == home || path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path;
}

QString ExpandHome(const QString& path) {
  return path == "~" || path.startsWith("~/") ? QDir::homePath() + path.mid(1) : path;
}

QString Counted(qsizetype count, const QString& item, const QString& plural) {
  return QString("%1 %2").arg(count).arg(count == 1         ? item
                                         : plural.isEmpty() ? item + "s"
                                                            : plural);
}

QString ShownOf(qsizetype shown, qsizetype games, qsizetype apps) {
  const qsizetype total = games + apps;
  const QString noun = apps == 0    ? (total == 1 ? "game" : "games")
                       : games == 0 ? (total == 1 ? "app" : "apps")
                                    : "games and apps";
  return QString("%1 of %2 %3").arg(shown).arg(total).arg(noun);
}

QString JoinAnd(QStringList items) {
  if (items.size() < 2) return items.join(QString());
  const QString last = items.takeLast();
  return items.join(", ") + " and " + last;
}

}  // namespace mira_gui
