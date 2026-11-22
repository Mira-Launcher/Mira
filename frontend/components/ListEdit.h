#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

// Makes `row` one item of a list under a setting's row, as ListEdit's: sunken, past the label of
// the row above, and ending where the controls of the rows around it do (a trailing button's
// glyph when `button_last`, else its text). Returns the row's layout.
QHBoxLayout* ListItemRow(QWidget* row, bool button_last = true);
// The remove button at the end of a list's item row.
QToolButton* NewRemoveButton(QWidget* parent);

// A list setting's items as rows of their own under its row, each editable in place with a
// remove button. Its row shows EmptyLabel and AddButton. For folders, Add opens a folder
// picker; otherwise it adds a row to type into. A long list shows its first few items and a
// "Show all" row.
class ListEdit : public QWidget {
  Q_OBJECT

 public:
  enum class Kind { Text, Folder };
  ListEdit(Kind kind, const QString& add_text, QWidget* parent = nullptr);

  QStringList Items() const;
  void SetItems(const QStringList& items);
  // For the row the list is under: "None" while empty, and the Add button.
  QLabel* EmptyLabel() const { return empty_; }
  QPushButton* AddButton() const { return add_; }
  // Items shown read-only and muted, without remove buttons, e.g. a list a game takes from
  // Settings.
  void SetReadOnly(bool read_only);

  static constexpr int kShortList = 6;  // longer lists show kShownFolded items until opened
  static constexpr int kShownFolded = 3;

 signals:
  void Changed();

 private:
  void AddRow(const QString& text, bool edit);
  void Update();

  Kind kind_;
  bool read_only_ = false;
  bool show_all_ = false;
  QVBoxLayout* rows_ = nullptr;
  QLabel* empty_ = nullptr;
  QPushButton* add_ = nullptr;
  QPushButton* more_ = nullptr;
};

}  // namespace mira_gui
