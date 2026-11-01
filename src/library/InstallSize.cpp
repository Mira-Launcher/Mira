#include "library/InstallSize.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <json.hpp>
#include <map>
#include <mutex>
#include <regex>

#include "amazon/Nile.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"

namespace mira::library {
namespace {

using nlohmann::json;

std::mutex cache_mutex;
std::map<std::pair<std::string, std::string>, InstallSize> cache;
// Titles a session can look at; past this one is dropped for each new one, and asked again if
// needed.
constexpr size_t kCacheLimit = 500;

std::optional<InstallSize> SizeIn(const json& value) {
  if (value.is_object()) {
    if (value.contains("download_size") || value.contains("disk_size")) {
      InstallSize size;
      if (value.contains("download_size") && value["download_size"].is_number())
        size.download_bytes = value["download_size"].get<std::int64_t>();
      if (value.contains("disk_size") && value["disk_size"].is_number())
        size.install_bytes = value["disk_size"].get<std::int64_t>();
      return size;
    }
    if (value.contains("*")) {
      if (auto size = SizeIn(value["*"])) return size;
    }
  }
  if (value.is_object() || value.is_array()) {
    for (const json& child : value) {
      if (auto size = SizeIn(child)) return size;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<InstallSize> ParseInstallSize(std::string_view output) {
  // The tools log to the same stream; the JSON is what's between the outer braces.
  const size_t begin = output.find('{');
  const size_t end = output.rfind('}');
  if (begin == std::string_view::npos || end == std::string_view::npos || end < begin)
    return std::nullopt;
  const json parsed = json::parse(output.substr(begin, end - begin + 1), nullptr, false);
  if (parsed.is_discarded()) return std::nullopt;
  return SizeIn(parsed);
}

Result<InstallSize> FetchInstallSize(const config::Config& config, const std::string& source,
                                     const std::string& ref) {
  if (auto known = KnownInstallSize(source, ref)) return *known;
  const auto ask = [&]() -> Result<std::string> {
    if (source == "epic") return epic::RunLegendary(config, {"info", ref, "--json"});
    if (source == "gog") return gog::RunGogdl(config, {"info", ref, "--platform", "windows"});
    if (source == "amazon") return amazon::RunNile(config, {"install", "--info", "--json", ref});
    return Err("size_unsupported", source + " doesn't say how big a title is before installing");
  };
  const Result<std::string> output = ask();
  if (!output) return std::unexpected(output.error());
  const auto size = ParseInstallSize(*output);
  if (!size) return Err("size_unknown", "The store didn't say how big " + ref + " is");
  std::lock_guard lock(cache_mutex);
  if (cache.size() >= kCacheLimit && !cache.contains({source, ref})) cache.erase(cache.begin());
  cache[{source, ref}] = *size;
  return *size;
}

std::int64_t EstimatedInstallBytes(const json& metadata) {
  static const std::regex kTag("<[^>]*>");
  static const std::regex kStorage(
      R"((?:storage|hard\s*(?:drive|disk)(?:\s*space)?|disk\s*space|hdd)\s*:\s*([0-9]+(?:[.,][0-9]+)?)\s*(tb|gb|mb))",
      std::regex::icase);
  if (!metadata.is_object() || !metadata.contains("steam") || !metadata["steam"].is_object())
    return -1;
  const json requirements = metadata["steam"].value("pc_requirements", json::object());
  std::int64_t largest = -1;
  for (const char* level : {"minimum", "recommended"}) {
    if (!requirements.is_object() || !requirements.contains(level) ||
        !requirements[level].is_string())
      continue;
    const std::string text = std::regex_replace(requirements[level].get<std::string>(), kTag, " ");
    std::smatch match;
    if (!std::regex_search(text, match, kStorage)) continue;
    std::string number = match[1].str();
    std::ranges::replace(number, ',', '.');
    const char unit = static_cast<char>(std::tolower(match[2].str().front()));
    // In the units clients show sizes in, so "15 GB" reads back as 15 GB.
    const double scale = std::pow(1024.0, unit == 't' ? 4 : unit == 'g' ? 3 : 2);
    largest = std::max(largest, static_cast<std::int64_t>(std::stod(number) * scale));
  }
  return largest;
}

std::optional<InstallSize> KnownInstallSize(const std::string& source, const std::string& ref) {
  std::lock_guard lock(cache_mutex);
  const auto found = cache.find({source, ref});
  if (found == cache.end()) return std::nullopt;
  return found->second;
}

}  // namespace mira::library
