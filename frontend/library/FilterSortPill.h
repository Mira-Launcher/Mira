#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <string>

class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;

namespace mira_gui {

// The library's filter and sort in one pill: "Ready | Name ↑ ⌄". A click
// opens a popover with the filter list (each with a live count) and the
// sort rows. The popover is a Qt::Popup, so it closes itself on an outside
// click or Escape.
class FilterSortPill : public QWidget {
  Q_OBJECT

 public:
  FilterSortPill(const std::string& sort_key, bool sort_descending, QWidget* parent);
  ~FilterSortPill() override;

  // Filter keys match mirad's `status` values; "all", "running", "never",
  // "hidden", "attention" and "apps" are the frontend's own groupings.
  QString FilterKey() const;
  QStringList FilterKeys() const;
  // -1 for a key it doesn't list.
  int FilterRow(const QString& key) const;
  void SetFilterRow(int row);
  void SetCount(const QString& key, int count);

  // A tag the Tags section offers, with how many games under the current filter have it.
  struct TagCount {
    QString tag;
    int count = 0;
  };
  // The tags offered, in order; a picked one that's no longer offered stays picked until cleared.
  void SetTags(const QList<TagCount>& tags);
  QStringList PickedTags() const { return picked_; }
  void SetPickedTags(const QStringList& tags);
  // Back to all games with no tags picked; a middle click on the pill does this too.
  void ClearFilters();

  const std::string& SortKey() const { return sort_key_; }
  bool SortDescending() const { return sort_descending_; }

 signals:
  void FilterChanged();
  void SortChanged();
  void TagsChanged();

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  QWidget* BuildPopover();
  // The pill's text and icons, and the popover's rows, in the theme's colors.
  void UpdateSummary();
  void RestyleFilterRows();
  void ApplyIcons();

  std::string sort_key_;
  bool sort_descending_ = false;
  QLabel* filter_icon_ = nullptr;
  QLabel* filter_label_ = nullptr;
  QLabel* sort_icon_ = nullptr;
  QLabel* sort_label_ = nullptr;
  QLabel* chevron_ = nullptr;
  QWidget* popover_ = nullptr;
  QListWidget* filters_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  QList<QPushButton*> sort_buttons_;
  QWidget* tags_heading_ = nullptr;
  QWidget* tags_box_ = nullptr;
  QWidget* tags_divider_ = nullptr;
  QToolButton* clear_tags_ = nullptr;
  QList<TagCount> tags_;
  QStringList picked_;
  void RebuildTags();
};

}  // namespace mira_gui
