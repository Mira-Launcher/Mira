#pragma once

#include <QLabel>
#include <QWidget>

namespace mira_gui {

class Card;
class ColumnGrid;

// One settings page, a section of the settings scroll (SettingsNavWidget): a title over its cards
// in as many columns as fit, up to three, starting where a full page's first column does
// (ColumnGrid).
class SettingsPage : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsPage(const QString& title, QWidget* parent = nullptr);

  Card* AddCard(const QString& title);
  void AddWidget(QWidget* widget);
  // With `columns` columns, `widget` goes in column `column`, in the order placed; a page that
  // places nothing flows its cards down one column and on into the next.
  void Place(QWidget* widget, int columns, int column);
  QString Title() const;
  QLabel* TitleLabel() const { return title_; }
  // A small heading above the title, for the first page of a nav group.
  void SetGroupHeading(const QString& text);
  // Splits the cards into columns again from their current heights.
  void Rearrange();
  // Where the cards start, from the page's left edge, and how wide they span.
  int GridLeft() const;
  int GridWidth() const;

 private:
  ColumnGrid* grid_ = nullptr;
  QLabel* title_ = nullptr;
};

}  // namespace mira_gui
