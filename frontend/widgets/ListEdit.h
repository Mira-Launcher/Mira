#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QVBoxLayout;

namespace mira_gui {

// A list setting as rows with a remove button each, plus an "Add" row. For
// folders, Add opens a folder picker; otherwise it adds a row to type into.
class ListEdit : public QWidget {
  Q_OBJECT

public:
  enum class Kind { Text, Folder };
  ListEdit(Kind kind, const QString& add_text, QWidget* parent = nullptr);

  QStringList Items() const;
  void SetItems(const QStringList& items);

signals:
  void Changed();

private:
  void AddRow(const QString& text, bool edit);
  void UpdateEmpty();

  Kind kind_;
  QVBoxLayout* rows_ = nullptr;
  QWidget* empty_ = nullptr;
};

}  // namespace mira_gui
