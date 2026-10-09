#pragma once

#include <QString>

#include <array>
#include <set>
#include <string>

namespace mira_gui {

// Steam's own review labels as the not-installed filters group them, best first. The four
// negative labels share one group; titles with too few reviews for a label are NotRated.
enum class ReviewBucket { OverwhelminglyPositive, VeryPositive, Positive, MostlyPositive, Mixed, Negative, NotRated };

inline constexpr std::array<ReviewBucket, 7> kReviewBuckets = {
    ReviewBucket::OverwhelminglyPositive, ReviewBucket::VeryPositive, ReviewBucket::Positive,
    ReviewBucket::MostlyPositive,         ReviewBucket::Mixed,        ReviewBucket::Negative,
    ReviewBucket::NotRated,
};

ReviewBucket ReviewBucketOf(const std::string& score_description);
QString ReviewBucketName(ReviewBucket bucket);

// ProtonDB's tiers as the filters list them, best first; "" is not rated yet (pending or none).
inline constexpr std::array<const char*, 7> kProtonDbFilterTiers = {"native", "platinum", "gold", "silver",
                                                                    "bronze", "borked",   ""};
// A title's tier as one of kProtonDbFilterTiers.
std::string ProtonDbFilterTier(const std::string& tier);
QString ProtonDbTierName(const std::string& filter_tier);

// What the not-installed filters let through: each set empty for any, else one of its values.
struct TitleFilter {
  std::set<std::string> stores;
  std::set<std::string> tiers;  // kProtonDbFilterTiers values
  std::set<ReviewBucket> reviews;

  bool Empty() const { return stores.empty() && tiers.empty() && reviews.empty(); }
  bool Matches(const std::string& store, const std::string& protondb_tier, const std::string& review_summary) const;
};

}  // namespace mira_gui
