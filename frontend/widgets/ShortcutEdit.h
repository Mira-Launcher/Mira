#pragma once

#include <QKeySequence>
#include <QWidget>

namespace mira_gui {

// A shortcut drawn as keycaps. Click it (or press Enter or Space on it) and
// press the new keys to rebind it; Esc cancels, Backspace leaves it with none.
class ShortcutEdit : public QWidget {
  Q_OBJECT

public:
  explicit ShortcutEdit(const QKeySequence& keys, QWidget* parent = nullptr);

  QKeySequence Keys() const { return keys_; }
  void SetKeys(const QKeySequence& keys);
  QSize sizeHint() const override;

signals:
  void Changed(const QKeySequence& keys);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  bool event(QEvent* event) override;

private:
  void SetRecording(bool recording);
  QStringList Caps() const;

  QKeySequence keys_;
  bool recording_ = false;
};

}  // namespace mira_gui
