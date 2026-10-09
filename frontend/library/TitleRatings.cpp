#include "TitleRatings.h"

#include <algorithm>

namespace mira_gui {

ReviewBucket ReviewBucketOf(const std::string& score_description) {
  if (score_description == "Overwhelmingly Positive") return ReviewBucket::OverwhelminglyPositive;
  if (score_description == "Very Positive") return ReviewBucket::VeryPositive;
  if (score_description == "Positive") return ReviewBucket::Positive;
  if (score_description == "Mostly Positive") return ReviewBucket::MostlyPositive;
  if (score_description == "Mixed") return ReviewBucket::Mixed;
  if (score_description.ends_with("Negative")) return ReviewBucket::Negative;
  // "3 user reviews", or none at all.
  return ReviewBucket::NotRated;
}

QString ReviewBucketName(ReviewBucket bucket) {
  switch (bucket) {
    case ReviewBucket::OverwhelminglyPositive: return "Overwhelmingly Positive";
    case ReviewBucket::VeryPositive: return "Very Positive";
    case ReviewBucket::Positive: return "Positive";
    case ReviewBucket::MostlyPositive: return "Mostly Positive";
    case ReviewBucket::Mixed: return "Mixed";
    case ReviewBucket::Negative: return "Negative";
    case ReviewBucket::NotRated: return "Not rated";
  }
  return {};
}

std::string ProtonDbFilterTier(const std::string& tier) {
  if (tier == "garbage") return "borked";
  const bool listed = std::ranges::any_of(kProtonDbFilterTiers, [&tier](const char* known) { return tier == known; });
  return listed ? tier : std::string();
}

QString ProtonDbTierName(const std::string& filter_tier) {
  if (filter_tier.empty()) return "Not rated yet";
  QString name = QString::fromStdString(filter_tier);
  name[0] = name[0].toUpper();
  return name;
}

bool TitleFilter::Matches(const std::string& store, const std::string& protondb_tier,
                          const std::string& review_summary) const {
  return (stores.empty() || stores.contains(store)) &&
         (tiers.empty() || tiers.contains(ProtonDbFilterTier(protondb_tier))) &&
         (reviews.empty() || reviews.contains(ReviewBucketOf(review_summary)));
}

}  // namespace mira_gui
