#pragma once

#include <QFrame>

class QLabel;
class QPushButton;

namespace mira_gui {

// "2 unsaved changes  [Discard] [Save]", floating at the bottom of `over` and
// only shown while there is something to save. RoomChanged tells the scrolls under
// it how much room to keep at their bottom, so it never covers a row.
class ChangeBar : public QFrame {
  Q_OBJECT

 public:
  explicit ChangeBar(QWidget* over);
  void SetCount(int count);
  // Another message and button labels, e.g. for a picked cover. Empty `text` hides the bar.
  void SetText(const QString& text, const QString& save = "Save",
               const QString& discard = "Discard");
  void SetBusy(bool busy);  // while a save runs
  // The space it takes at the bottom of `over`, gap included.
  int RoomNeeded() const;

 signals:
  void DiscardClicked();
  void SaveClicked();
  // The room the bar takes at the bottom of `over` now, 0 while hidden.
  void RoomChanged(int room);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void Place();

  QLabel* text_ = nullptr;
  QPushButton* discard_ = nullptr;
  QPushButton* save_ = nullptr;
};

}  // namespace mira_gui
