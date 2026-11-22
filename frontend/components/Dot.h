#pragma once

#include <QWidget>

#include "Variants.h"

class QLabel;

namespace mira_gui {

// A round mark in a tone's color, as wide as half a line of `line_of`'s text (its own font's when
// null) and as tall as a whole line, with the dot on the line's middle. Repaints on a theme change.
class Dot : public QWidget {
  Q_OBJECT

 public:
  explicit Dot(Tone tone = Tone::Muted, QWidget* parent = nullptr, QWidget* line_of = nullptr);

  void SetTone(Tone tone);
  Tone CurrentTone() const { return tone_; }

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  int Diameter() const;

  Tone tone_;
  QWidget* line_of_;
};

// A state, a dot in its tone before its text ("● Live", "● Not signed in · 12 games in your
// library"). The dot sits on the text's first line when it wraps.
class StatusLine : public QWidget {
  Q_OBJECT

 public:
  explicit StatusLine(QWidget* parent = nullptr);

  void Set(Tone tone, const QString& text);
  // Text alone, for a line with no state to show.
  void SetPlain(const QString& text);
  QLabel* Label() const { return label_; }
  Dot* Mark() const { return dot_; }

 private:
  Dot* dot_;
  QLabel* label_;
};

}  // namespace mira_gui
