#pragma once

#include <QLabel>
#include <QWidget>

namespace mira_gui {

class Card;

// One settings page, a section of the settings scroll (SettingsNavWidget): a
// title over its cards in one column centered in the page (SettingsColumnWidth).
class SettingsPage : public QWidget {
  Q_OBJECT

 public:
  explicit SettingsPage(const QString& title, QWidget* parent = nullptr);

  Card* AddCard(const QString& title);
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

}  // namespace mira_gui
