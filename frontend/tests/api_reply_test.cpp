#include <doctest.h>

#include <json.hpp>

#include "client/api/Request.h"

using nlohmann::json;
using namespace mira_gui;

namespace {

struct NamesResult {
  bool ok = false;
  ApiError error;
  std::vector<std::string> names;
};

void FillNames(NamesResult& result, const json& body) {
  for (const json& entry : body) result.names.push_back(entry.value("name", std::string()));
}

transport::Reply Answer(json body) {
  transport::Reply reply;
  reply.ok = true;
  reply.status = 200;
  reply.body = std::move(body);
  return reply;
}

}  // namespace

TEST_CASE("ReadReply hands a well-formed body to its reader") {
  const NamesResult result = api::ReadReply<NamesResult>(
      Answer(json::parse(R"([{"name": "Celeste"}, {"name": "Hades"}])")), "GET /names",
      api::Shape::Array, FillNames);
  CHECK(result.ok);
  CHECK(result.names == std::vector<std::string>{"Celeste", "Hades"});
}

TEST_CASE("ReadReply passes a failed request's error through") {
  transport::Reply reply;
  reply.error = ApiError("mirad isn't running", ApiError::kUnreachable);
  const NamesResult result =
      api::ReadReply<NamesResult>(reply, "GET /names", api::Shape::Array, FillNames);
  CHECK_FALSE(result.ok);
  CHECK(result.error.code == ApiError::kUnreachable);
}

TEST_CASE("ReadReply turns a body of the wrong shape into an error, not a crash") {
  const NamesResult result = api::ReadReply<NamesResult>(
      Answer(json::parse(R"({"name": "x"})")), "GET /names", api::Shape::Array, FillNames);
  CHECK_FALSE(result.ok);
  CHECK(result.error.message.find("GET /names") != std::string::npos);
}

TEST_CASE("ReadReply turns a field of the wrong type into an error with nothing half-read") {
  // nlohmann's value() throws when the key exists with another type.
  const NamesResult result =
      api::ReadReply<NamesResult>(Answer(json::parse(R"([{"name": "Celeste"}, {"name": 7}])")),
                                  "GET /names", api::Shape::Array, FillNames);
  CHECK_FALSE(result.ok);
  CHECK(result.names.empty());
  CHECK_FALSE(result.error.empty());
}
