#include <doctest.h>

#include "ui/SettingsSearch.h"

using namespace mira_gui::settings_search;

namespace {

bool Finds(const char* row_text, const char* query) {
  return Matches(Normalize(QString::fromUtf8(row_text)), QString::fromUtf8(query));
}

}  // namespace

TEST_CASE("Settings search matches words in any order") {
  CHECK(Finds("steam.web_api_key Steam Web API Key Metadata", "steam key"));
  CHECK(Finds("steam.web_api_key Steam Web API Key Metadata", "key steam"));
  CHECK_FALSE(Finds("steam.web_api_key Steam Web API Key Metadata", "steam gog"));
}

TEST_CASE("Settings search treats key punctuation as spaces") {
  CHECK(Finds("Needed alongside steam.web_api_key", "api key"));
  CHECK(Finds("launch.stop_timeout_s", "stop timeout"));
}

TEST_CASE("Settings search matches a word across spaces") {
  CHECK(Finds("Steam ID (64-bit)", "steamid"));
  CHECK(Finds("SteamGridDB API Key", "apikey"));
}

TEST_CASE("Settings search: empty query matches everything, case is ignored") {
  CHECK(Finds("anything", ""));
  CHECK(Finds("anything", "   "));
  CHECK(Finds("Theme appearance", "THEME"));
}
