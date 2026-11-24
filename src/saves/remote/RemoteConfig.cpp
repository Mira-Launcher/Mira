#include "saves/remote/RemoteConfig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <initializer_list>
#include <json.hpp>
#include <mutex>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::saves::remote {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

using Values = std::map<std::string, std::string>;

constexpr std::size_t kMaxPassword = 1024;

std::mutex& ConfigLock() {
  static std::mutex guard;
  return guard;
}

std::unexpected<Error> Invalid(const char* message) {
  return Err("invalid_remote", message);
}

std::string Get(const Values& values, const char* key) {
  const auto it = values.find(key);
  return it == values.end() ? std::string{} : it->second;
}

bool HasControl(std::string_view text) {
  return std::ranges::any_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// rclone.conf trims a value and reads a leading quote or backtick as quoting it.
bool Storable(std::string_view value) {
  constexpr std::string_view kSpecial = " \"`";
  return value.empty() || (!kSpecial.contains(value.front()) && value.back() != ' ');
}

bool KeysAllowed(const Values& values, std::initializer_list<std::string_view> allowed) {
  return std::ranges::all_of(values, [&](const auto& pair) {
    return std::ranges::find(allowed, std::string_view(pair.first)) != allowed.end();
  });
}

bool IsFile(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

bool IsNonEmptyFile(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}

bool IsAbsoluteFile(const std::string& path) {
  return fs::path(path).is_absolute() && IsFile(path);
}

// Relative, and no empty, "." or ".." part.
bool FolderOk(const std::string& folder) {
  std::size_t start = 0;
  while (!folder.empty()) {
    const std::size_t end = folder.find('/', start);
    const std::string part = folder.substr(start, end == std::string::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return folder.empty() || !fs::path(folder).is_absolute();
}

bool HttpsUrl(const std::string& url) {
  const std::string lower = strings::ToLower(url);
  if (!lower.starts_with("https://") || url.find_first_of("@ ?#") != std::string::npos)
    return false;
  const std::string rest = lower.substr(8);
  return !rest.substr(0, rest.find('/')).empty();
}

bool SftpHost(const std::string& host) {
  return !host.empty() && host[0] != '-' && std::ranges::all_of(host, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == ':' || c == '-' || c == '_';
  });
}

bool ValidPort(const std::string& port) {
  if (port.size() > 5 ||
      !std::ranges::all_of(port, [](unsigned char c) { return std::isdigit(c); }))
    return false;
  int number = 0;
  std::from_chars(port.data(), port.data() + port.size(), number);
  return number >= 1 && number <= 65535;
}

// rclone's obscured form of `password`; the password goes on stdin, never in argv.
Result<std::string> Obscure(const config::Config& config, const std::string& password) {
  const runner::ToolStatus tool = DetectRclone(config);
  if (!tool.installed) return Err("rclone_missing", "rclone isn't installed.");
  Command command;
  command.argv = {tool.path, "--config", RcloneConfigFile(config).string(), "--log-level", "ERROR",
                  "obscure", "-"};
  command.input = password;
  command.unset_prefixes = {"RCLONE_"};
  const auto run = runner::RunAndWait(command);
  if (!run) return std::unexpected(run.error());
  const std::string value = strings::Trim(run->output);
  const bool plain = std::ranges::all_of(
      value, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
  if (run->exit_code != 0 || value.empty() || !plain)
    return Err("remote_failed", "rclone couldn't store the password.");
  return value;
}

fs::path RemotesFile(const config::Config& config) {
  return config.File().parent_path() / "remotes.json";
}

json LoadRemotes(const config::Config& config) {
  const auto text = files::ReadFile(RemotesFile(config));
  json doc = text ? json::parse(*text, nullptr, /*allow_exceptions=*/false) : json::object();
  return doc.is_object() ? doc : json::object();
}

Result<std::string> ReadRcloneConfig(const fs::path& file) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return std::string{};
  auto text = files::ReadFile(file);
  if (!text) return Err("remote_config_failed", "couldn't read rclone's settings.");
  return *text;
}

// The text without the sections named `slot`; every other byte is kept.
std::string WithoutSection(std::string_view text, std::string_view slot) {
  const std::string header = std::format("[{}]", slot);
  std::string kept;
  bool skipping = false;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    end = end == std::string_view::npos ? text.size() : end + 1;
    const std::string_view line = text.substr(pos, end - pos);
    if (line.starts_with('['))
      skipping = line.substr(0, line.size() - line.ends_with('\n')) == header;
    if (!skipping) kept.append(line);
    pos = end;
  }
  return kept;
}

std::string NewId() {
  std::random_device random;
  return std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}

json Entry(const RemoteInfo& info) {
  return {{"id", info.id},
          {"provider", info.provider},
          {"name", info.name},
          {"folder", info.folder},
          {"ca_file", info.ca_file}};
}

}  // namespace

Result<void> CheckRequest(const RemoteRequest& request) {
  const Values& values = request.values;
  const std::string& provider = request.provider;
  if (request.slot != "primary" && request.slot != "mirror")
    return Invalid("The slot must be primary or mirror.");
  if (provider != "webdav" && provider != "sftp" && provider != "s3")
    return Invalid("The provider must be webdav, sftp or s3.");
  if (request.name.empty() || request.name.size() > 64)
    return Invalid("The name must be 1 to 64 bytes.");
  if (HasControl(request.slot) || HasControl(provider) || HasControl(request.name) ||
      HasControl(request.folder) || HasControl(request.ca_file))
    return Invalid("A field holds a control character.");
  for (const auto& [key, value] : values) {
    if (HasControl(key) || HasControl(value)) return Invalid("A value holds a control character.");
    if (key != "password" && !Storable(value))
      return Invalid("A value can't start with a space or quote, or end with a space.");
  }
  if (!FolderOk(request.folder))
    return Invalid("The folder must be relative, with no empty, . or .. part.");
  if (!request.ca_file.empty() && (provider == "sftp" || !IsAbsoluteFile(request.ca_file)))
    return Invalid("The certificate must be an existing absolute file, for webdav or s3.");
  if (values.contains("password") && values.at("password").size() > kMaxPassword)
    return Invalid("The password is too long.");

  if (provider == "webdav") {
    if (!KeysAllowed(values, {"url", "user", "password"}))
      return Invalid("An unknown setting was given.");
    if (!HttpsUrl(Get(values, "url")))
      return Invalid("The URL must be https:// with a host, and no @, space, ? or #.");
  } else if (provider == "sftp") {
    if (!KeysAllowed(values, {"host", "port", "user", "key_file", "password", "known_hosts_file"}))
      return Invalid("An unknown setting was given.");
    if (!SftpHost(Get(values, "host"))) return Invalid("The host name isn't valid.");
    const std::string port = Get(values, "port");
    if (!port.empty() && !ValidPort(port)) return Invalid("The port must be 1 to 65535.");
    if (Get(values, "user").empty()) return Invalid("A user name is required.");
    const std::string key_file = Get(values, "key_file");
    if (key_file.empty() && Get(values, "password").empty())
      return Invalid("A key file or a password is required.");
    if (!key_file.empty() && !IsAbsoluteFile(key_file))
      return Invalid("The key file must be an existing absolute file.");
    const std::string known_hosts = Get(values, "known_hosts_file");
    if (!fs::path(known_hosts).is_absolute() || !IsNonEmptyFile(known_hosts))
      return Invalid("An absolute known hosts file that isn't empty is required.");
  } else {
    if (!KeysAllowed(values, {"endpoint", "region", "access_key_id", "secret_access_key"}))
      return Invalid("An unknown setting was given.");
    if (!HttpsUrl(Get(values, "endpoint")))
      return Invalid("The endpoint must be https:// with a host, and no @, space, ? or #.");
    if (Get(values, "access_key_id").empty() || Get(values, "secret_access_key").empty())
      return Invalid("An access key ID and a secret access key are required.");
    if (request.folder.empty()) return Invalid("A folder is required, starting with the bucket.");
  }
  return {};
}

fs::path RcloneConfigFile(const config::Config& config) {
  return config.File().parent_path() / "rclone.conf";
}

Result<RemoteInfo> SaveRemote(const config::Config& config, const RemoteRequest& request) {
  if (const auto checked = CheckRequest(request); !checked) return std::unexpected(checked.error());
  const Values& values = request.values;
  std::string obscured;
  if (!Get(values, "password").empty()) {
    const auto stored = Obscure(config, Get(values, "password"));
    if (!stored) return std::unexpected(stored.error());
    obscured = *stored;
  }

  RemoteInfo info{.slot = request.slot,
                  .id = NewId(),
                  .provider = request.provider,
                  .name = request.name,
                  .folder = request.folder,
                  .ca_file = request.ca_file};
  std::vector<std::pair<std::string, std::string>> keys;
  if (request.provider == "webdav") {
    keys = {{"type", "webdav"},
            {"url", Get(values, "url")},
            {"vendor", "other"},
            {"user", Get(values, "user")},
            {"pass", obscured}};
  } else if (request.provider == "sftp") {
    const std::string port = Get(values, "port");
    keys = {{"type", "sftp"},
            {"host", Get(values, "host")},
            {"port", port.empty() ? "22" : port},
            {"user", Get(values, "user")},
            {"key_file", Get(values, "key_file")},
            {"pass", obscured},
            {"known_hosts_file", Get(values, "known_hosts_file")},
            {"shell_type", "none"}};
  } else {
    keys = {{"type", "s3"},
            {"provider", "Other"},
            {"endpoint", Get(values, "endpoint")},
            {"region", Get(values, "region")},
            {"access_key_id", Get(values, "access_key_id")},
            {"secret_access_key", Get(values, "secret_access_key")},
            {"env_auth", "false"}};
  }
  std::string section = std::format("[{}]\n", request.slot);
  for (const auto& [key, value] : keys)
    if (!value.empty()) section += std::format("{} = {}\n", key, value);

  const std::lock_guard lock(ConfigLock());
  const fs::path conf = RcloneConfigFile(config);
  const auto text = ReadRcloneConfig(conf);
  if (!text) return std::unexpected(text.error());
  std::string next = WithoutSection(*text, request.slot);
  if (!next.empty() && next.back() != '\n') next += '\n';
  next += section;

  json doc = LoadRemotes(config);
  doc[request.slot] = Entry(info);
  if (auto written = WriteFileAtomic(conf, next, "remote_config_failed", true, 0600); !written)
    return std::unexpected(written.error());
  if (auto written =
          WriteFileAtomic(RemotesFile(config), doc.dump(2), "remote_config_failed", true, 0600);
      !written)
    return std::unexpected(written.error());
  return info;
}

std::optional<RemoteInfo> FindRemote(const config::Config& config, std::string_view slot) {
  const std::lock_guard lock(ConfigLock());
  const json doc = LoadRemotes(config);
  const auto it = doc.find(std::string(slot));
  if (it == doc.end() || !it->is_object()) return std::nullopt;
  RemoteInfo info;
  info.slot = std::string(slot);
  info.id = core::JsonString(*it, "id");
  info.provider = core::JsonString(*it, "provider");
  info.name = core::JsonString(*it, "name");
  info.folder = core::JsonString(*it, "folder");
  info.ca_file = core::JsonString(*it, "ca_file");
  return info;
}

Result<void> ForgetRemote(const config::Config& config, std::string_view slot) {
  const std::lock_guard lock(ConfigLock());
  const fs::path conf = RcloneConfigFile(config);
  const auto text = ReadRcloneConfig(conf);
  if (!text) return std::unexpected(text.error());
  const std::string next = WithoutSection(*text, slot);
  if (next != *text) {
    if (auto written = WriteFileAtomic(conf, next, "remote_config_failed", true, 0600); !written)
      return std::unexpected(written.error());
  }

  json doc = LoadRemotes(config);
  if (doc.erase(std::string(slot)) > 0) {
    if (auto written =
            WriteFileAtomic(RemotesFile(config), doc.dump(2), "remote_config_failed", true, 0600);
        !written)
      return std::unexpected(written.error());
  }
  return {};
}

Remote RemoteFor(const config::Config& config, const RemoteInfo& info) {
  return {.config_file = RcloneConfigFile(config),
          .root = info.slot + ":" + info.folder,
          .ca_file = info.ca_file};
}

}  // namespace mira::saves::remote
