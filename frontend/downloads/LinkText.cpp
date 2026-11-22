#include "LinkText.h"

#include <QRegularExpression>
#include <algorithm>

namespace mira_gui::downloads {

std::vector<std::string> ParseLinks(const QString& text) {
  std::vector<std::string> links;
  static const QRegularExpression kSpace("\\s+");
  for (const QString& word : text.split(kSpace, Qt::SkipEmptyParts)) {
    if (!word.startsWith("http://", Qt::CaseInsensitive) &&
        !word.startsWith("https://", Qt::CaseInsensitive) &&
        !word.startsWith("magnet:", Qt::CaseInsensitive))
      continue;
    std::string link = word.toStdString();
    if (!std::ranges::contains(links, link)) links.push_back(std::move(link));
  }
  return links;
}

}  // namespace mira_gui::downloads
