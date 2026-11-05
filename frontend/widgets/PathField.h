#pragma once

#include <QString>
#include <filesystem>
#include <functional>

class QLineEdit;
class QWidget;

namespace mira_gui {

// `selected` relative to `base` when it is inside `base`, else `selected` itself.
QString RelativeIfInside(const QString& selected, const std::filesystem::path& base);

// Scrolls `edit` back to the start of its path, which reads like the rest of the fields; the whole
// path is its tooltip.
void ShowPathStart(QLineEdit* edit);

// A row of `edit` plus a "Browse…" button; `pick` opens the dialog and returns the chosen path or
// "" (cancelled).
QWidget* PathRow(QLineEdit* edit, std::function<QString()> pick);

}  // namespace mira_gui
