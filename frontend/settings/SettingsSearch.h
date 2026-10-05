#pragma once

#include <QString>

namespace mira_gui::settings_search {

// Lowercases and turns key punctuation ("steam.web_api_key") into spaces, so
// a row's text and a query compare word for word.
QString Normalize(const QString& text);

// True when every word of `query` appears in `normalized_text` (from Normalize),
// in any order. A word also matches across spaces, so "steamid" finds "Steam ID".
bool Matches(const QString& normalized_text, const QString& query);

}  // namespace mira_gui::settings_search
