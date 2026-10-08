#pragma once

#include <QStringList>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QFrame;
class QLineEdit;
class QPushButton;

namespace mira_gui {

// A game's tags as chips, each with its own remove button, then "Add tag",
// which turns into a field for a new one. The chips wrap onto more lines and
// show in the library's tag order (SetOrder), whatever order the game stores.
// The game's folder tag is outlined with a folder icon, and its other folder
// tags carry a muted one: a click on one of those (or its menu) picks it as
// this game's folder over the folder tags' order. A click on any other chip
// does nothing; its menu can show the games with that tag.
class TagEdit : public QWidget {
  Q_OBJECT

public:
  explicit TagEdit(QWidget* parent = nullptr);

  void SetTags(const std::vector<std::string>& tags);
  const std::vector<std::string>& Tags() const { return tags_; }
  // Offered as the new tag is typed.
  void SetSuggestions(const QStringList& tags);
  // The library's tags in the order the chips follow (TagOrder).
  void SetOrder(std::vector<std::string> order);
  // The folder tags of the game's library folder, in their order, or none while it isn't sorted.
  void SetFolderTags(std::optional<std::vector<std::string>> folder_tags);
  // The folder tag picked for this game (GameSummary::folder_tag); empty to follow the order.
  void SetFolderPick(const std::string& pick);
  const std::string& FolderPick() const { return pick_; }

signals:
  void Changed();
  // A chip's menu asked to show the games with this tag.
  void TagClicked(const QString& tag);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void Rebuild();
  void StartAdding();
  void FinishAdding();
  bool IsFolderTag(const std::string& tag) const;
  // The tag the game's folder is in, or empty.
  std::string FolderTag() const;
  // Makes `tag` the game's folder: no pick when the order already makes it so.
  void PickFolder(const std::string& tag);
  void Remove(const std::string& tag);
  void ShowMenu(const std::string& tag, const QPoint& global);

  std::vector<std::string> tags_;
  std::vector<std::string> order_;
  std::optional<std::vector<std::string>> folder_tags_;
  std::string pick_;
  QList<QFrame*> chips_;
  QPushButton* add_ = nullptr;
  QLineEdit* input_ = nullptr;
  QFrame* pressed_ = nullptr;  // a press on a chip, until its release makes it a click
};

}  // namespace mira_gui
