#pragma once

#include <QStringList>
#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;
class QPushButton;

namespace mira_gui {

// A game's tags as chips, each with its own remove button, then "Add tag",
// which turns into a field for a new one. The chips wrap onto more lines.
class TagEdit : public QWidget {
  Q_OBJECT

public:
  explicit TagEdit(QWidget* parent = nullptr);

  void SetTags(const std::vector<std::string>& tags);
  const std::vector<std::string>& Tags() const { return tags_; }
  // Offered as the new tag is typed.
  void SetSuggestions(const QStringList& tags);

signals:
  void Changed();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void Rebuild();
  void StartAdding();
  void FinishAdding();

  std::vector<std::string> tags_;
  QList<QWidget*> chips_;
  QPushButton* add_ = nullptr;
  QLineEdit* input_ = nullptr;
};

}  // namespace mira_gui
