#pragma once

#include <QLineEdit>

namespace mira_gui {

// A field to search or filter with: a search glyph before the text and a clear button after it
// while there's text, both Mira's own glyphs.
class SearchField : public QLineEdit {
  Q_OBJECT

 public:
  explicit SearchField(const QString& placeholder, QWidget* parent = nullptr);
};

// Gives `field` Mira's clear button, shown while it has text, in place of the desktop's own icon
// (setClearButtonEnabled), which doesn't match the rest.
void AddClearButton(QLineEdit* field, const QString& tip = "Clear");

}  // namespace mira_gui
