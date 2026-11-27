#pragma once

#include <QWidget>

#include "Variants.h"

namespace mira_gui {

// A small outlined label beside a name ("Beta", "Default", "Flatpak", "Saves in the cloud"): its
// text and border in the tone's color, muted by default. Not a control: it takes no focus or
// clicks.
class Badge : public QWidget {
  Q_OBJECT

 public:
  explicit Badge(const QString& text, Tone tone = Tone::Muted, QWidget* parent = nullptr);

  void SetText(const QString& text);
  QString Text() const { return text_; }
  void SetTone(Tone tone);
  // A color of its own in place of the tone's (a ProtonDB tier); an invalid color goes back to it.
  void SetColor(const QColor& color);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  QFont TextFont() const;

  QString text_;
  Tone tone_ = Tone::Muted;
  QColor color_;
};

}  // namespace mira_gui
