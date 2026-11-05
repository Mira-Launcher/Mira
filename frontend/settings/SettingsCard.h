#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QWidget>
#include <functional>
#include <vector>

class QHBoxLayout;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QVariantAnimation;

namespace mira_gui {

// Every control's height in a card (base.qss), so a row is as tall whatever it holds.
constexpr int kControlHeight = 32;

// An on/off control drawn as a sliding switch, the settings rows' toggle.
class Switch : public QAbstractButton {
  Q_OBJECT

 public:
  explicit Switch(QWidget* parent = nullptr);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  qreal position_ = 0;  // 0 is off, 1 is on; animated between the two
  QVariantAnimation* slide_ = nullptr;
};

// One setting in a card: the label, which shows its doc as a tooltip, then its
// controls on the right. While the value has an unsaved change, a dot marks the
// label and an undo button puts back the saved value.
class SettingRow : public QWidget {
  Q_OBJECT

 public:
  SettingRow(const QString& label, const QString& doc, QWidget* parent = nullptr);

  // Right-aligned, in the order added.
  void AddControl(QWidget* control, int stretch = 0);
  // A control that takes all the width the label leaves, e.g. a line of its own text, placed
  // right after the label.
  void AddWideControl(QWidget* control);
  // Full width under the label, e.g. a folder list.
  void SetBelow(QWidget* widget);
  // Under the row from one edge of its card to the other, e.g. a list's items as rows of their own.
  void SetBelowFlush(QWidget* widget);
  // Moves the label right, for a row inside an ExpanderRow.
  void SetIndent(int pixels);
  // Widgets placed between the label and the controls, e.g. a source's kind tag.
  void AddAfterLabel(QWidget* widget);
  // A widget just before the label, e.g. a source's colored initial.
  void SetLeading(QWidget* widget);

  QLabel* Label() const { return label_; }
  // A handle before the label for dragging the row to a new place in its card.
  void ShowGrip();
  // Shows or hides that handle, keeping its space so the labels stay lined up.
  void SetGripShown(bool shown);
  QWidget* Grip() const { return grip_; }
  // Shows the dot and the undo button. The owner connects RevertClicked.
  void SetModified(bool modified);
  // A saved value of its own, such as a game's override: the undo button without the dot, to put
  // Settings' value back.
  void SetOverridden(bool overridden);
  // Shows `other`'s changes and overrides as this row's too, for a setting shown inside this row.
  void Include(SettingRow* other);

 signals:
  void RevertClicked();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void ShowRevert();

  SettingRow* included_in_ = nullptr;
  std::vector<SettingRow*> included_;
  QWidget* dot_ = nullptr;
  QWidget* grip_ = nullptr;
  QLabel* label_ = nullptr;
  QTimer* doc_delay_ = nullptr;  // the label's tooltip shows sooner than Qt's own
  QToolButton* revert_ = nullptr;
  bool modified_ = false;
  bool overridden_ = false;
  QHBoxLayout* line_ = nullptr;  // dot, label, extras, stretch, revert, controls
  QVBoxLayout* outer_ = nullptr;
  int label_end_ = 0;  // where AddAfterLabel inserts
};

// A row of a card that opens in place to rows of its own, for something with settings of its own
// (an emulator, a store, a download): its name, a summary, then a chevron. Its rows sit on a
// sunken surface, indented.
class ExpanderRow : public QWidget {
  Q_OBJECT

 public:
  explicit ExpanderRow(const QString& label, QWidget* parent = nullptr);

  void AddRow(QWidget* row);
  const QList<QWidget*>& Rows() const { return rows_; }
  // Muted text after the label, e.g. the consoles an emulator runs.
  void SetMeta(const QString& text);
  // On the right before the chevron, e.g. "Installed"; `tone` "success" colors it.
  void SetSummary(const QString& text, const QString& tone = {});
  // Before the chevron, after the summary, e.g. a source's on switch.
  void AddTrailing(QWidget* widget);
  void SetLeading(QWidget* widget);
  void SetExpanded(bool expanded);
  bool Expanded() const;
  QLabel* Label() const { return label_; }

 signals:
  void Toggled(bool expanded);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  QWidget* head_ = nullptr;
  QHBoxLayout* head_layout_ = nullptr;
  QLabel* label_ = nullptr;
  QLabel* meta_ = nullptr;
  QLabel* summary_ = nullptr;
  QToolButton* chevron_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  QList<QWidget*> rows_;
};

// The width of the one column settings and a game's card show their cards in, for `room` px.
int SettingsColumnWidth(int room);

// The gap under a page's title, on the page and in the header that sticks while scrolling.
constexpr int kTitleGap = 12;

// Where a row's label starts in its card (the line's margin, the dot and the spaces after it), and
// so where a card's title and anything else under it start.
constexpr int kLabelInset = 8 + 6 + 6 + 4;
// A text field in a row, with its buttons (Browse…, Open), so every field starts at one line.
constexpr int kFieldColumnWidth = 340;
// A number box in a row, wide enough for its unit, so a card's boxes line up on both edges.
constexpr int kNumberWidth = 120;
// `field` stretched to fill kFieldColumnWidth beside `buttons`.
QWidget* FieldWithButtons(QWidget* field, const std::vector<QWidget*>& buttons);

// A titled group of rows on a raised surface, with a line between rows. Can
// start folded, opening from its header.
class SettingsCard : public QFrame {
  Q_OBJECT

 public:
  explicit SettingsCard(const QString& title, QWidget* parent = nullptr);

  void AddRow(QWidget* row);
  // Removes and deletes every row, for a list that is rebuilt.
  void ClearRows();
  // Removes and deletes one row.
  void RemoveRow(QWidget* row);
  // The header, for trailing widgets such as a switch or a button.
  QHBoxLayout* Header() const { return header_layout_; }
  // A widget before the title, e.g. a source's colored initial.
  void SetLeading(QWidget* widget);
  // Retitles the card; empty hides the header.
  void SetTitle(const QString& title);
  // Replaces the plain title with a richer one (e.g. a name over a status line).
  void SetTitleWidget(QWidget* widget);
  // A heading-sized title, for a card that stands alone over the window.
  void SetProminentTitle();
  void SetCollapsible(bool collapsed);
  // Muted text in the header before the chevron, e.g. "12 emulators · 3 installed".
  void SetSummary(const QString& text);
  void SetExpanded(bool expanded);
  bool Expanded() const;
  bool Collapsible() const { return chevron_ != nullptr; }
  const QList<QWidget*>& Rows() const { return rows_; }
  QString Title() const;
  // Moves `row` to position `to` among the card's rows.
  void MoveRow(QWidget* row, int to);
  // The rows scroll inside the card, which then takes the height its layout gives it.
  void SetScrollable();
  // A row between the header and the rows that stays put while they scroll, e.g. column headings.
  void SetPinnedRow(QWidget* row);
  // A row under the rows that stays put while they scroll, e.g. the card's buttons.
  void AddFooterRow(QWidget* row);
  // The scroll the rows are in, once SetScrollable made one.
  QScrollArea* Scroll() const { return scroll_; }
  // A "Reset to defaults" button in the header, shown while `resettable`.
  void SetResettable(bool resettable);

 signals:
  // A row with a grip (SettingRow::ShowGrip) was dragged, or moved with Alt+Up/Down. It only trades
  // places with other rows that have one.
  void RowsReordered();
  void ResetClicked();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  bool event(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  bool HandleGrip(QWidget* grip, QEvent* event);
  // Gives a sunken row (a list's, an opened expander's) that ends the card the card's rounded
  // bottom corners, so its square ones don't poke past them.
  void RoundBottomRow();

  QWidget* dragging_ = nullptr;  // the row being dragged by its grip
  bool dragged_ = false;
  QWidget* header_ = nullptr;
  QHBoxLayout* header_layout_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* summary_ = nullptr;
  QToolButton* chevron_ = nullptr;
  QToolButton* reset_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* pinned_ = nullptr;
  QWidget* footer_ = nullptr;
  QVBoxLayout* footer_layout_ = nullptr;
  QList<QWidget*> rows_;
};

// One settings page, a section of the settings scroll (SettingsNavWidget): a
// title over its cards in one column centered in the page (SettingsColumnWidth).
class SettingsPage : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsPage(const QString& title, QWidget* parent = nullptr);

  SettingsCard* AddCard(const QString& title);
  void AddWidget(QWidget* widget);
  QString Title() const;
  QLabel* TitleLabel() const { return title_; }
  // A small heading above the title, for the first page of a nav group.
  void SetGroupHeading(const QString& text);
  // Lays the cards out again from their current heights.
  void Rearrange();
  // Where the column starts, from the page's left edge.
  int ColumnLeft() const;

 private:
  QLabel* title_ = nullptr;
};

// "2 unsaved changes  [Discard] [Save]", floating at the bottom of `over` and
// only shown while there is something to save. While shown, the settings
// scrolls under `over` get room at their bottom so it never covers a row.
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

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void Place();

  QLabel* text_ = nullptr;
  QPushButton* discard_ = nullptr;
  QPushButton* save_ = nullptr;
};

}  // namespace mira_gui
