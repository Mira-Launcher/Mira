#pragma once

#include <QColor>
#include <QDateTime>
#include <QLocale>
#include <QString>

#include "../client/Types.h"
#include "Theme.h"

#include <cstdint>
#include <optional>
#include <string>

// Small presentation helpers shared by the grid, the sidebar and a game's
// card, so a game reads the same way everywhere.
namespace mira_gui {

// A finished bulk refresh, e.g. "Refreshed 180 games. 20 found nothing."
inline QString BatchRefreshSummary(const MetadataBatchResult& result) {
  QString text = result.refreshed == 1 ? QString("Refreshed 1 game.")
                                       : QString("Refreshed %1 games.").arg(result.refreshed);
  if (result.failed > 0) text += QString(" %1 found nothing.").arg(result.failed);
  return text;
}

// A color per lifecycle state so status reads at a glance without a
// legend. The theme owns the colors; see ui/Theme.h.
inline QColor StatusColor(const std::string& status) {
  const theme::Tokens& tokens = theme::Current();
  if (status == "running") return tokens.running;  // not a mirad status: "playing right now"
  if (status == "ready") return tokens.status_ready;
  if (status == "setting_up") return tokens.status_setting_up;
  if (status == "broken") return tokens.status_broken;
  if (status == "missing") return tokens.status_missing;
  if (status == "needs_install") return tokens.status_needs_install;
  return tokens.text_muted;
}

// ProtonDB's own tier colors, read out of their production site's own
// bundle (head.protondb.pages.dev/static/js/main.*.js, theme.colors.medals)
// rather than approximated. "native" is deliberately not part of that
// `medals` map on their side either: it lives as its own top-level
// `native: "green"` entry in the same object, because it means "doesn't
// touch Proton at all" rather than grading how well Proton runs it, which
// is the distinction ProtonDbTierIsNative below exists to carry into ours.
inline QColor ProtonDbTierColor(const std::string& tier) {
  if (tier == "platinum") return QColor("#b4c7dc");
  if (tier == "gold") return QColor("#cfb53b");
  if (tier == "silver") return QColor("#a6a6a6");
  if (tier == "bronze") return QColor("#cd7f32");
  if (tier == "borked" || tier == "garbage") return QColor("#ff0000");
  if (tier == "native") return QColor("#008000");
  return QColor("#444444");  // "pending"
}

inline bool ProtonDbTierIsNative(const std::string& tier) { return tier == "native"; }

// Platinum/gold/silver are light enough that white text on them would wash
// out; the rest are dark enough that black text would.
inline QColor ContrastingTextColor(const QColor& background) {
  const double luminance =
      0.299 * background.red() + 0.587 * background.green() + 0.114 * background.blue();
  return luminance > 140 ? QColor("#1c1f25") : QColor("#ffffff");
}

// Shared so the same game reads identically in the
// grid, its hover card, and its edit page.
inline QString StatusLabel(const std::string& status) {
  if (status == "needs_install") return "Needs install";
  if (status == "setting_up") return "Setting up";
  QString label = QString::fromStdString(status);
  if (!label.isEmpty()) label[0] = label[0].toUpper();
  return label;
}

inline QString FormatLastPlayed(const std::optional<std::int64_t>& last_played_at) {
  if (!last_played_at) return "Never";
  return QDateTime::fromSecsSinceEpoch(*last_played_at).toString("yyyy-MM-dd HH:mm");
}

// "Today", "Yesterday", "3 days ago", then the date.
inline QString FormatPlayedAgo(const std::optional<std::int64_t>& last_played_at) {
  if (!last_played_at) return "Never played";
  const QDate played = QDateTime::fromSecsSinceEpoch(*last_played_at).date();
  const qint64 days = played.daysTo(QDate::currentDate());
  if (days <= 0) return "Today";
  if (days == 1) return "Yesterday";
  if (days < 7) return QString("%1 days ago").arg(days);
  return QLocale().toString(played, QLocale::ShortFormat);
}

// FormatPlayedAgo short enough for a small cover: "Today", "3d ago", then "12 Sep".
inline QString FormatPlayedAgoShort(const std::optional<std::int64_t>& last_played_at) {
  if (!last_played_at) return "Never";
  const QDate played = QDateTime::fromSecsSinceEpoch(*last_played_at).date();
  const qint64 days = played.daysTo(QDate::currentDate());
  if (days <= 0) return "Today";
  if (days < 7) return QString("%1d ago").arg(days);
  return QLocale().toString(played, "d MMM");
}

inline QString FormatPlaytime(std::int64_t play_seconds) {
  if (play_seconds <= 0) return "0m";
  const std::int64_t hours = play_seconds / 3600;
  const std::int64_t minutes = (play_seconds % 3600) / 60;
  if (hours > 0) return QString("%1h %2m").arg(hours).arg(minutes);
  if (minutes > 0) return QString("%1m").arg(minutes);
  return "<1m";
}

}  // namespace mira_gui
