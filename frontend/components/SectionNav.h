#pragma once

#include <QObject>
#include <vector>

class QAbstractButton;
class QButtonGroup;
class QScrollArea;
class QVariantAnimation;
class QWidget;

namespace mira_gui {

// A list of buttons, one per section of a scroll: a click glides the scroll to that section's top,
// and the checked button follows the scroll to the section in view. Settings' nav and a game card's
// jump list. A hidden section is skipped both ways.
class SectionNav : public QObject {
  Q_OBJECT

 public:
  // `clearance`: the space left over a section's top once a jump has scrolled to it.
  SectionNav(QScrollArea* scroll, int clearance, QObject* parent = nullptr);

  // `button` jumps to `section`, a widget inside the scroll.
  void AddSection(QAbstractButton* button, QWidget* section);
  // Glides to section `index`, its button checked meanwhile.
  void Select(int index);
  // Stops a glide where it is.
  void Stop();
  // The lowest section whose top has reached where a jump puts it (sections can sit side by side).
  int CurrentIndex() const;
  // Checks the current section's button, e.g. after the sections changed size.
  void Sync();
  int Count() const { return static_cast<int>(sections_.size()); }

 private:
  struct Section {
    QAbstractButton* button = nullptr;
    QWidget* widget = nullptr;
  };
  int TopOf(const QWidget* widget) const;

  QScrollArea* scroll_;
  int clearance_;
  QButtonGroup* buttons_;
  QVariantAnimation* glide_;
  int jumping_to_ = -1;  // the section a glide is heading for
  std::vector<Section> sections_;
};

}  // namespace mira_gui
