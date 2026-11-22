#pragma once

#include <QString>
#include <functional>

#include "Card.h"

class QHBoxLayout;
class QLabel;
class QToolButton;

namespace mira_gui {

// A card that stands alone over the window or in a dialog (installing a game, About, a source's
// setup): a heading with a close button kept last, a back arrow for a page opened from its first,
// and under the rows an error line and a row of buttons.
class SheetCard : public Card {
  Q_OBJECT

 public:
  explicit SheetCard(const QString& title = {}, QWidget* parent = nullptr);

  // A heading-sized title, with a muted `subtitle` under it unless that's empty. Returns the title.
  QLabel* SetHeading(const QString& title, const QString& subtitle = {});
  // The close button, last in the header, with `tip`, which runs `close`.
  void SetClose(const QString& tip, const std::function<void()>& close);
  // A back arrow before the title.
  QToolButton* SetBack(const std::function<void()>& back);
  // A red line under the rows, above the buttons, hidden until there's an error to show.
  QLabel* ErrorLine();
  // The card's last row: buttons on the right, after a stretch.
  QHBoxLayout* ButtonRow();

 private:
  QLabel* error_ = nullptr;
  QHBoxLayout* buttons_ = nullptr;
};

}  // namespace mira_gui
