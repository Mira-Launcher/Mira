#include <doctest.h>

#include "../bigscreen/NavRepeater.h"

using namespace mira_gui::bigscreen;

namespace {

bool Has(const std::vector<Nav>& out, Nav nav) {
  for (Nav n : out) {
    if (n == nav) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("NavRepeater presses once on the way down and repeats only directions while held") {
  NavRepeater r;
  std::array<bool, size_t(Nav::kCount)> down{};
  down[size_t(Nav::Accept)] = true;
  down[size_t(Nav::Down)] = true;

  auto first = r.Feed(down, 0);
  CHECK(first.size() == 2);
  CHECK(Has(first, Nav::Down));
  CHECK(Has(first, Nav::Accept));

  CHECK(r.Feed(down, 100).empty());

  auto repeat = r.Feed(down, NavRepeater::kFirstRepeatMs);
  CHECK(repeat.size() == 1);
  CHECK(Has(repeat, Nav::Down));

  auto next = r.Feed(down, NavRepeater::kFirstRepeatMs + NavRepeater::kRepeatMs);
  CHECK(next.size() == 1);
  CHECK(Has(next, Nav::Down));

  down = {};
  CHECK(r.Feed(down, 2000).empty());

  down[size_t(Nav::Accept)] = true;
  auto again = r.Feed(down, 2010);
  CHECK(again.size() == 1);
  CHECK(Has(again, Nav::Accept));
}
