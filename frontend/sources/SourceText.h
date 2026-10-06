#pragma once

#include <QString>
#include <string>
#include <utility>
#include <vector>

#include "../client/ApiError.h"

class QLabel;

namespace mira_gui {

// What differs per source, in words. Endpoints live in client/api/Stores.
struct SourceCopy {
  QString blurb;  // one line under the page title
  QString tool;   // stores: the helper mirad drives
  QString sign_in_steps;
  QString credential_placeholder;
  QString import_button;  // empty: no import
  QString item = "game";   // what it imports, singular
  // A launcher made of several apps, (ref, name) each, that can be installed separately: Microsoft 365.
  std::vector<std::pair<QString, QString>> parts;
};

SourceCopy CopyFor(const std::string& id);

// Sets `label` to `text` in base.qss's `role`, hidden while empty.
void ShowLine(QLabel* label, const QString& text, const char* role);
// "<what> <mirad's message and hint>" as an error line. mirad's messages
// start lowercase; this one follows a sentence.
void ShowError(QLabel* label, const QString& what, const ApiError& error);

// An import's outcome, e.g. "Added 3 games."
QString ImportOutcome(int added, int updated);
// A section heading with its count after it, dimmed.
QString CountedHeading(const QString& text, int count);

}  // namespace mira_gui
