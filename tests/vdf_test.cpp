#include "steam/Vdf.h"

#include <doctest.h>

using namespace mira;

TEST_CASE("ParseVdf reads nested objects and scalars, case-insensitively") {
  const char* text = R"(
"libraryfolders"
{
	"0"
	{
		"path"		"/home/user/.local/share/Steam"
		"apps"
		{
			"228980"		"128606066"
			"1493710"		"1537166031"
		}
	}
}
)";
  auto root = steam::ParseVdf(text);
  REQUIRE(root.has_value());

  const auto* path = root->Get({"libraryfolders", "0", "path"});
  REQUIRE(path != nullptr);
  CHECK(path->scalar == "/home/user/.local/share/Steam");

  const auto* app = root->Get({"LibraryFolders", "0", "Apps", "228980"});
  REQUIRE(app != nullptr);
  CHECK(app->scalar == "128606066");

  CHECK(root->Get({"libraryfolders", "0", "apps", "nonexistent"}) == nullptr);
}

TEST_CASE("ParseVdf skips // comments") {
  const char* text = R"(
"AppState"
{
	// this is a comment
	"appid"		"228980"	// trailing comment
	"name"		"Steamworks Common Redistributables"
}
)";
  auto root = steam::ParseVdf(text);
  REQUIRE(root.has_value());
  CHECK(root->Get({"appstate", "appid"})->scalar == "228980");
  CHECK(root->Get({"appstate", "name"})->scalar == "Steamworks Common Redistributables");
}

TEST_CASE("ParseVdf handles the real localconfig.vdf Playtime/LastPlayed shape") {
  const char* text = R"(
"UserLocalConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"apps"
				{
					"480"
					{
						"LastPlayed"		"1759204559"
						"Playtime"		"332"
					}
				}
			}
		}
	}
}
)";
  auto root = steam::ParseVdf(text);
  REQUIRE(root.has_value());
  const auto* app =
      root->Get({"UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "480"});
  REQUIRE(app != nullptr);
  CHECK(app->Get({"Playtime"})->scalar == "332");
  CHECK(app->Get({"LastPlayed"})->scalar == "1759204559");
}

TEST_CASE("ParseVdf rejects unterminated input instead of hanging or crashing") {
  CHECK_FALSE(steam::ParseVdf(R"("a" { "b" "c")").has_value());
  CHECK_FALSE(steam::ParseVdf(R"("a" "unterminated)").has_value());
}
