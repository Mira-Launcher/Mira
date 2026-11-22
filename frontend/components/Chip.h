#pragma once

#include <QAbstractButton>
#include <QPointer>

class QMenu;

namespace mira_gui {

// A pill-shaped button: a filter or choice that turns on (checkable, solid accent while on), a
// dropdown (SetMenu, with a chevron), or a removable item (SetRemovable, with an ×). It can show a
// muted count after its text and an icon before it. Marked draws an accent outline, e.g. a tag that
// comes from a folder.
class Chip : public QAbstractButton {
  Q_OBJECT

 public:
  explicit Chip(const QString& text = {}, QWidget* parent = nullptr);

  // A muted number after the text; below 0 shows none.
  void SetCount(int count);
  // Opens `menu` under the chip on a click, and shows a chevron.
  void SetMenu(QMenu* menu);
  QMenu* Menu() const;
  // Shows an × that emits RemoveClicked instead of a click, as Delete and Backspace do.
  void SetRemovable(bool removable, const QString& tip = {});
  void SetMarked(bool marked);
  // A color of its own (a ProtonDB tier, a Steam review group): the chip is tinted with it while
  // off and filled with it while on, in place of the accent. An invalid color is the plain look.
  void SetTint(const QColor& tint);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override { return sizeHint(); }

 signals:
  void RemoveClicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool event(QEvent* event) override;
  bool hitButton(const QPoint& pos) const override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  struct Layout {
    QRectF icon, text, count, end;  // end: the chevron or the ×
  };
  Layout LayOut() const;
  int TrailingSize() const;

  int count_ = -1;
  QPointer<QMenu> menu_;
  bool removable_ = false;
  QString remove_tip_;
  bool marked_ = false;
  QColor tint_;
  bool remove_pressed_ = false;
};

}  // namespace mira_gui
