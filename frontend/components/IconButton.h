#pragma once

#include <QToolButton>
#include <QVariantAnimation>

#include "../theme/Icons.h"

namespace mira_gui {

// A frameless button showing one glyph (back, close, more, settings, open a link), redrawn in a new
// theme's `role` color. `tip` is its tooltip and its accessible name.
class IconButton : public QToolButton {
  Q_OBJECT

 public:
  IconButton(icons::Glyph glyph, const QString& tip, QWidget* parent = nullptr,
             icons::Role role = &theme::Tokens::text);

  // Shows `glyph` from now on, in this and later themes.
  void SetGlyph(icons::Glyph glyph);

  // Turns the glyph round and round, for work going on; off finishes the turn it's on.
  void SetSpinning(bool spinning);

 private:
  void Apply();
  void ApplyAngle(qreal degrees);

  icons::Glyph glyph_;
  icons::Role role_;
  QVariantAnimation* spin_ = nullptr;
};

}  // namespace mira_gui
