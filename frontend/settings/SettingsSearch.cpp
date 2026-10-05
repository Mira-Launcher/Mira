#include "SettingsSearch.h"

#include <QRegularExpression>
#include <QStringList>

namespace mira_gui::settings_search {

QString Normalize(const QString& text) {
  static const QRegularExpression kSeparators(R"([\s._\-/()'",:;]+)");
  return text.toLower().replace(kSeparators, " ").trimmed();
}

bool Matches(const QString& normalized_text, const QString& query) {
  const QStringList words = Normalize(query).split(' ', Qt::SkipEmptyParts);
  if (words.isEmpty()) return true;
  QString compact = normalized_text;
  compact.remove(' ');
  for (const QString& word : words) {
    if (!normalized_text.contains(word) && !compact.contains(word)) return false;
  }
  return true;
}

}  // namespace mira_gui::settings_search
