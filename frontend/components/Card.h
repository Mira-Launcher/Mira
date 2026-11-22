#pragma once

#include <QFrame>
#include <QLabel>
#include <QList>

class QHBoxLayout;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

// A titled group of rows on a raised surface, with a line between rows. Can
// start folded, opening from its header.
class Card : public QFrame {
  Q_OBJECT

 public:
  explicit Card(const QString& title, QWidget* parent = nullptr);

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

}  // namespace mira_gui
