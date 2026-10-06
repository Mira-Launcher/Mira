#include <doctest.h>

#include "core/LogHub.h"

using namespace mira;

TEST_CASE("Each channel keeps its own lines, read from a cursor") {
  loghub::Begin("test:a");
  loghub::Begin("test:b");
  loghub::Append("test:a", "one\ntwo\npart");
  loghub::Append("test:b", "other\n");

  const loghub::Page first = loghub::Read("test:a", nullptr, 10);
  CHECK(first.lines == std::vector<std::string>{"one", "two"});  // "part" waits for its newline
  CHECK(first.active);
  CHECK(loghub::Read("test:b", nullptr, 10).lines == std::vector<std::string>{"other"});

  loghub::Append("test:a", "ial\n");
  const loghub::Page next = loghub::Read("test:a", &first.next, 10);
  CHECK(next.lines == std::vector<std::string>{"partial"});

  loghub::End("test:a");
  CHECK_FALSE(loghub::Read("test:a", &next.next, 10).active);
  CHECK(loghub::Read("test:a", nullptr, 1).lines == std::vector<std::string>{"partial"});
}

TEST_CASE("Starting a channel over drops its old lines") {
  loghub::Begin("test:c");
  loghub::Append("test:c", "old\n");
  const loghub::Page before = loghub::Read("test:c", nullptr, 10);
  loghub::Begin("test:c");
  loghub::Append("test:c", "new\n");
  CHECK(loghub::Read("test:c", &before.next, 10).lines == std::vector<std::string>{"new"});
}
