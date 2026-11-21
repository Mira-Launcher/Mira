#pragma once

#include <QString>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../library/GamePicker.h"

class QLabel;
class QLineEdit;

namespace mira_gui {

class Switch;

// The Tags page's game picker: covers to tick, the games a tag would go on. Sections come first
// (e.g. the games Steam gives the tag, ticked), then every other game unticked, with a search.
// Hidden games are ticked as they would be elsewhere; store launchers aren't offered, and keep
// the tag as they had it. Applying makes exactly the ticked games have the tag
// (POST /v1/tags/set).
class TagPicker : public GamePicker {
  Q_OBJECT

 public:
  struct Section {
    QString heading;
    std::vector<std::string> ids;
    bool ticked = false;
  };

  explicit TagPicker(ArtworkStore* artwork, QWidget* parent = nullptr);

  // `name` empty asks for one. `existing` is true for a tag the library has: then Save sends the
  // ticked set as is, and the folder switch reads "A folder". `known` is the library's tags, so a
  // typed name that's already a tag keeps the games that have it.
  void Open(const QString& name, bool existing, std::vector<Section> sections,
            const std::vector<GameSummary>& games, bool folder, std::vector<TagSummary> known);

 signals:
  // The tag was set; the records that changed, for the window to apply now.
  void Applied(const std::vector<GameSummary>& games);

 protected:
  void UpdateApply() override;
  void Apply() override;

 private:
  // The games that end with the tag: the ticked ones, and the launchers that had it.
  std::vector<std::string> Chosen() const;

  QLineEdit* name_edit_ = nullptr;
  Switch* folder_ = nullptr;
  QLabel* folder_label_ = nullptr;
  QString name_;
  bool existing_ = false;
  bool folder_was_ = false;
  std::vector<std::string> had_;   // the games that had the tag when the picker opened
  std::vector<std::string> kept_;  // the launchers among them, which aren't offered
  std::vector<TagSummary> known_;
};

}  // namespace mira_gui
