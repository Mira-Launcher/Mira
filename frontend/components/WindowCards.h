#pragma once

class QWidget;

namespace mira_gui {

// A window that lays cards over its content in a place of its own (LibraryWindow's card overlay,
// which keeps the sidebar bright). Any other window gets a dimmed overlay over all of it.
class CardHost {
 public:
  virtual ~CardHost() = default;
  // Shows `card`, replacing the one shown. False when the window can't leave what it shows now
  // (unsaved changes kept); the caller deletes the card then.
  virtual bool ShowCard(QWidget* card) = 0;
  virtual void CloseCard(QWidget* card) = 0;
};

namespace cards {

// Shows `card` over the window `from` is in, which takes it over. Esc or a click beside it
// closes it, as does Close.
void Show(QWidget* from, QWidget* card);
// Closes and deletes `card`, if it's still shown.
void Close(QWidget* card);

}  // namespace cards
}  // namespace mira_gui
