#pragma once

#include <QLabel>
#include <QList>
#include <QWidget>
#include <vector>

#include "Text.h"

class QHBoxLayout;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

// One setting in a card: the label, which shows its doc as a tooltip, then its
// controls on the right. While the value has an unsaved change, a dot marks the
// label and an undo button puts back the saved value.
class SettingRow : public QWidget {
  Q_OBJECT

 public:
  SettingRow(const QString& label, const QString& doc, QWidget* parent = nullptr);

  // Right-aligned, in the order added.
  void AddControl(QWidget* control, int stretch = 0);
  // A control that takes all the width the label leaves, e.g. a line of its own text, placed
  // right after the label.
  void AddWideControl(QWidget* control);
  // On a line of its own under the label, starting where the label does: a control too wide to
  // sit beside it, e.g. a path field with its buttons.
  void AddControlBelow(QWidget* control);
  // Full width under the label, e.g. a folder list.
  void SetBelow(QWidget* widget);
  // Under the row from one edge of its card to the other, e.g. a list's items as rows of their own.
  void SetBelowFlush(QWidget* widget);
  // Moves the label right, for a row inside an ExpanderRow.
  void SetIndent(int pixels);
  // Widgets placed between the label and the controls, e.g. a source's kind tag.
  void AddAfterLabel(QWidget* widget);
  // A widget just before the label, e.g. a source's colored initial.
  void SetLeading(QWidget* widget);

  QLabel* Label() const { return label_; }
  // A handle before the label for dragging the row to a new place in its card.
  void ShowGrip();
  // Shows or hides that handle, keeping its space so the labels stay lined up.
  void SetGripShown(bool shown);
  QWidget* Grip() const { return grip_; }
  // Shows the dot and the undo button. The owner connects RevertClicked.
  void SetModified(bool modified);
  // A saved value of its own, such as a game's override: the undo button without the dot, to put
  // Settings' value back.
  void SetOverridden(bool overridden);
  // Gives up the undo button's space, for a row that isn't a saved setting.
  void DropUndo();
  // Shows `other`'s changes and overrides as this row's too, for a setting shown inside this row.
  void Include(SettingRow* other);

 signals:
  void RevertClicked();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  // Where the label starts inside the row's line.
  int LabelLeft() const;
  // A line under the label holding `widget`, `left` px in from the line's start, `gap` px below it.
  QHBoxLayout* AddLineBelow(QWidget* widget, int left, int gap, int stretch);
  void ShowRevert();

  SettingRow* included_in_ = nullptr;
  std::vector<SettingRow*> included_;
  QWidget* dot_ = nullptr;
  QWidget* grip_ = nullptr;
  QLabel* label_ = nullptr;
  QTimer* doc_delay_ = nullptr;  // the label's tooltip shows sooner than Qt's own
  QToolButton* revert_ = nullptr;
  bool modified_ = false;
  bool overridden_ = false;
  QHBoxLayout* line_ = nullptr;  // dot, label, extras, stretch, revert, controls
  QVBoxLayout* outer_ = nullptr;
  int label_end_ = 0;  // where AddAfterLabel inserts
};

// A row of a card that opens in place to rows of its own, for something with settings of its own
// (an emulator, a store, a download): its name, a summary, then a chevron. Its rows sit on a
// sunken surface, indented.
class ExpanderRow : public QWidget {
  Q_OBJECT

 public:
  explicit ExpanderRow(const QString& label, QWidget* parent = nullptr);

  void AddRow(QWidget* row);
  // Removes and deletes every row, for a list that is rebuilt.
  void ClearRows();
  const QList<QWidget*>& Rows() const { return rows_; }
  // Muted text after the label, e.g. the consoles an emulator runs.
  void SetMeta(const QString& text);
  // On the right before the chevron, e.g. "Installed", muted unless `tone` says otherwise.
  void SetSummary(const QString& text, Tone tone = Tone::Muted);
  // Before the chevron, after the summary, e.g. a source's on switch.
  void AddTrailing(QWidget* widget);
  void SetLeading(QWidget* widget);
  void SetExpanded(bool expanded);
  bool Expanded() const;
  QLabel* Label() const { return label_; }

 signals:
  void Toggled(bool expanded);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  QWidget* head_ = nullptr;
  QHBoxLayout* head_layout_ = nullptr;
  QLabel* label_ = nullptr;
  QLabel* meta_ = nullptr;
  QLabel* summary_ = nullptr;
  QToolButton* chevron_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  QList<QWidget*> rows_;
};

// `field` stretched beside `buttons`, for a row's AddControlBelow.
QWidget* FieldWithButtons(QWidget* field, const std::vector<QWidget*>& buttons);

}  // namespace mira_gui
