#pragma once

#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "TitleRatings.h"

class QPushButton;

namespace mira_gui {

// Not-installed titles filtered by chips: one row of ProtonDB tiers, one of Steam review groups,
// each chip with how many titles it holds. Chips in a row add up; the rows narrow each other.
class RatingChips : public QWidget {
  Q_OBJECT

public:
  explicit RatingChips(QWidget* parent = nullptr);

  struct Title {
    std::string protondb_tier;
    std::string review_summary;
  };
  void SetTitles(const std::vector<Title>& titles);
  const TitleFilter& Filter() const { return filter_; }
  void Clear();

signals:
  void Changed();

private:
  void Toggle();

  TitleFilter filter_;
  std::map<std::string, QPushButton*> tier_chips_;
  std::map<ReviewBucket, QPushButton*> review_chips_;
};

}  // namespace mira_gui
