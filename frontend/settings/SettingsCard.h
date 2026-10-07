#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QWidget>

class QHBoxLayout;
class QPushButton;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QVariantAnimation;

namespace mira_gui {

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
  // Full width under the label, e.g. a folder list.
  void SetBelow(QWidget* widget);
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

signals:
  void RevertClicked();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  QWidget* dot_ = nullptr;
  QWidget* grip_ = nullptr;
  QLabel* label_ = nullptr;
  QTimer* doc_delay_ = nullptr;  // the label's tooltip shows sooner than Qt's own
  QToolButton* revert_ = nullptr;
  QHBoxLayout* line_ = nullptr;      // dot, label, extras, stretch, revert, controls
  QVBoxLayout* outer_ = nullptr;
  int label_end_ = 0;                // where AddAfterLabel inserts
};

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

private:
  bool HandleGrip(QWidget* grip, QEvent* event);

  QWidget* dragging_ = nullptr;  // the row being dragged by its grip
  bool dragged_ = false;
  QWidget* header_ = nullptr;
  QHBoxLayout* header_layout_ = nullptr;
  QLabel* title_ = nullptr;
  QToolButton* chevron_ = nullptr;
  QToolButton* reset_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* pinned_ = nullptr;
  QList<QWidget*> rows_;
};

// One settings page, a section of the settings scroll (SettingsNavWidget): a
// title over its cards in as many columns as fit, each 600 to 760 px wide.
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
  // Splits the cards into columns again from their current heights.
  void Rearrange();

private:
  QLabel* title_ = nullptr;
  QWidget* cards_ = nullptr;
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
  void SetText(const QString& text, const QString& save = "Save", const QString& discard = "Discard");
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
