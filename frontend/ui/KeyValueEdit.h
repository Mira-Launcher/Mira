#pragma once

#include <QWidget>

#include <map>
#include <string>

class QVBoxLayout;

namespace mira_gui {

// Name and value pairs (a game's environment variables) as rows in one box, styled like
// ListEdit: two fields and a remove button per row, plus an "Add" row.
class KeyValueEdit : public QWidget {
  Q_OBJECT

public:
  explicit KeyValueEdit(const QString& add_text, QWidget* parent = nullptr);

  // Rows with an empty name are left out; a later row wins over an earlier one with the same name.
  std::map<std::string, std::string> Values() const;
  void SetValues(const std::map<std::string, std::string>& values);

signals:
  void Changed();

private:
  void AddRow(const QString& name, const QString& value, bool edit);
  void UpdateEmpty();

  QVBoxLayout* rows_ = nullptr;
  QWidget* empty_ = nullptr;
};

}  // namespace mira_gui
