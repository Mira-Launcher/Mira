#include "downloads/Service.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <random>
#include <regex>

#include "core/Log.h"
#include "core/Strings.h"
#include "runner/Curl.h"
#include "runner/Exec.h"

namespace mira::downloads {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kTorBoxBase = "https://api.torbox.app/v1/api";
constexpr std::uintmax_t kMaxTorrentFile = 16 * 1024 * 1024;

std::string TextOf(const json& item, const char* key) {
  const auto found = item.find(key);
  return found != item.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::int64_t SizeOf(const json& item) {
  const auto found = item.find("size");
  return found != item.end() && found->is_number() ? found->get<std::int64_t>() : 0;
}

// Files live under the torrent's own name; the game's folder takes that place.
std::vector<ServiceFile> ParseFiles(const json& files) {
  std::vector<ServiceFile> out;
  if (!files.is_array()) return out;
  for (const json& file : files) {
    if (!file.is_object() || !file.contains("id")) continue;
    out.push_back({.id = file["id"].dump(),
                   .path = SafeRelativePath(TextOf(file, "name")),
                   .size = SizeOf(file)});
  }
  if (out.empty()) return out;
  const auto slash = out.front().path.find('/');
  if (slash == std::string::npos) return out;
  const std::string root = out.front().path.substr(0, slash + 1);
  if (std::ranges::all_of(out, [&](const auto& file) { return file.path.starts_with(root); })) {
    for (ServiceFile& file : out) file.path.erase(0, root.size());
  }
  return out;
}

class TorBox : public Service {
 public:
  explicit TorBox(std::string key) : key_(std::move(key)) {}

  std::string Name() const override { return "TorBox"; }

  Result<std::string> Add(const std::string& link, bool only_if_cached) override {
    const bool torrent = Classify(link) == LinkKind::Torrent;
    std::vector<std::string> args;
    fs::path torrent_file;
    if (!torrent) {
      args = {"--data-urlencode", "link=" + link};
      if (only_if_cached) args.insert(args.end(), {"--data-urlencode", "add_only_if_cached=true"});
    } else {
      if (strings::ToLower(link).starts_with("magnet:")) {
        args = {"--form-string", "magnet=" + link};
      } else {
        std::random_device random;
        torrent_file = fs::temp_directory_path() / std::format("mira-{:08x}.torrent", random());
        auto fetched = runner::CurlDownload(link, torrent_file, kMaxTorrentFile);
        if (!fetched) return std::unexpected(fetched.error());
        args = {"-F", "file=@" + torrent_file.string()};
      }
      if (only_if_cached) args.insert(args.end(), {"--form-string", "add_only_if_cached=true"});
    }
    const auto answer =
        Call(torrent ? "/torrents/createtorrent" : "/webdl/createwebdownload", std::move(args));
    if (!torrent_file.empty()) {
      std::error_code ec;
      fs::remove(torrent_file, ec);
    }
    if (!answer) return std::unexpected(answer.error());
    const char* id_key = torrent ? "torrent_id" : "webdownload_id";
    if (!answer->is_object() || !answer->contains(id_key) || !(*answer)[id_key].is_number())
      return Err("service_failed", "TorBox didn't say which download it added");
    return std::format("{}:{}", torrent ? "torrents" : "webdl", (*answer)[id_key].dump());
  }

  Result<ServiceStatus> Status(const std::string& job) override {
    const auto [kind, id] = Split(job);
    const auto item = Call(std::format("/{}/mylist?id={}&bypass_cache=true", kind, id), {});
    if (!item) return std::unexpected(item.error());
    if (!item->is_object()) return Err("service_failed", "TorBox no longer has that download");
    return ParseTorBoxItem(*item);
  }

  Result<std::string> FileAddress(const std::string& job, const std::string& file) override {
    const auto [kind, id] = Split(job);
    const auto address = Call(std::format("/{}/requestdl?token={}&{}={}&file_id={}", kind, key_,
                                          kind == "torrents" ? "torrent_id" : "web_id", id, file),
                              {});
    if (!address) return std::unexpected(address.error());
    if (!address->is_string())
      return Err("service_failed", "TorBox didn't give a download address");
    return address->get<std::string>();
  }

  std::vector<LinkFile> Peek(const std::string& link) override {
    static const std::regex kHash("btih:([0-9a-fA-F]{40})");
    std::smatch match;
    if (!std::regex_search(link, match, kHash)) return {};
    const std::string hash = strings::ToLower(match[1].str());
    const auto data =
        Call(std::format("/torrents/checkcached?hash={}&format=object&list_files=true", hash), {});
    if (!data) {
      log::Warn("TorBox cache check: {}", data.error().message);
      return {};
    }
    if (!data->is_object() || !data->contains(hash)) return {};
    std::vector<LinkFile> out;
    for (const ServiceFile& file : ParseFiles((*data)[hash].value("files", json::array()))) {
      out.push_back({.url = "", .name = file.path, .size = file.size, .source = "TorBox (cached)"});
    }
    return out;
  }

 private:
  static std::pair<std::string, std::string> Split(const std::string& job) {
    const auto colon = job.find(':');
    return {job.substr(0, colon), colon == std::string::npos ? "" : job.substr(colon + 1)};
  }

  // Calls the API and answers its `data`. Messages never carry the address, which may hold the key.
  Result<json> Call(const std::string& route, std::vector<std::string> args) const {
    std::vector<std::string> argv = {"-sS", "--connect-timeout", "10", "--max-time", "60"};
    argv.insert(argv.end(), args.begin(), args.end());
    const std::string secrets = runner::CurlConfigLine("header", "Authorization: Bearer " + key_) +
                                runner::CurlConfigLine("user-agent", "Mira") +
                                runner::CurlConfigLine("url", kTorBoxBase + route);
    const auto result = runner::RunCurlWithSecrets(argv, secrets);
    if (!result || result->exit_code != 0)
      return Err("service_failed", "couldn't reach TorBox", kConnectionHint);
    const json answer = json::parse(result->output, nullptr, false);
    if (!answer.is_object()) return Err("service_failed", "TorBox gave an unreadable answer");
    if (answer.value("success", false)) return answer.value("data", json());
    const std::string code = TextOf(answer, "error");
    if (code == "DOWNLOAD_NOT_CACHED") return Err(kNotCached, "TorBox doesn't have it ready");
    if (code == "BAD_TOKEN" || code == "AUTH_ERROR" || code == "NO_AUTH")
      return Err(kServiceKeyRejected, "TorBox didn't accept the API key",
                 "Check the key in Settings, under Downloads.");
    const std::string detail = TextOf(answer, "detail");
    return Err("service_failed", std::format("TorBox: {}", detail.empty() ? code : detail));
  }

  std::string key_;
};

}  // namespace

std::string SafeRelativePath(const std::string& path) {
  std::string out;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = std::min(path.find_first_of("/\\", start), path.size());
    const std::string part = path.substr(start, end - start);
    start = end + 1;
    if (part.empty() || part.find_first_not_of(". ") == std::string::npos) continue;
    if (!out.empty()) out += '/';
    out += CleanFileName(part, "");
  }
  return out.empty() ? std::string("download") : out;
}

ServiceStatus ParseTorBoxItem(const json& item) {
  ServiceStatus status;
  const std::string state = strings::ToLower(TextOf(item, "download_state"));
  status.ready = item.value("download_finished", false) && item.value("download_present", false);
  const auto progress = item.find("progress");
  if (progress != item.end() && progress->is_number())
    status.progress = std::clamp(progress->get<double>(), 0.0, 1.0);
  status.failure = TextOf(item, "error");
  if (status.failure.empty() && !status.ready &&
      (state.starts_with("failed") || state.starts_with("error") || state == "expired"))
    status.failure = state == "expired" ? "it expired on TorBox" : state;
  if (item.contains("files")) status.files = ParseFiles(item["files"]);
  return status;
}

std::unique_ptr<Service> ActiveService(const config::Config& config) {
  const std::string key = config.GetString("downloads.torbox_key");
  if (key.empty()) return nullptr;
  return std::make_unique<TorBox>(key);
}

}  // namespace mira::downloads
