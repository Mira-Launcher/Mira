#include "saves/remote/Connect.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <json.hpp>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "model/Types.h"
#include "runner/Exec.h"
#include "saves/remote/Revisions.h"
#include "system/Packages.h"

namespace mira::saves::remote {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kSignInTimeoutS = 300;

fs::path Folder(const config::Config& config) {
  return config.File().parent_path();
}

std::string KnownHostsPrefix(std::string_view slot) {
  return std::format("saves-known-hosts-{}-", slot);
}

// Drops the slot's known hosts files but `keep`.
void DropKnownHosts(const config::Config& config, std::string_view slot, const fs::path& keep) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(Folder(config), ec)) {
    if (entry.path() == keep ||
        !entry.path().filename().string().starts_with(KnownHostsPrefix(slot)))
      continue;
    std::error_code ignored;
    fs::remove(entry.path(), ignored);
  }
}

// A name no other attempt uses.
std::string Unique() {
  return NewRevisionId(model::NowSeconds());
}

// Only lines of "<host> <type> <base64>".
bool HostKeysOk(std::string_view text) {
  if (text.empty() || text.size() > 16384) return false;
  bool any = false;
  for (const std::string& line : strings::Split(std::string(text), '\n')) {
    if (line.empty()) continue;
    const std::vector<std::string> parts = strings::Split(line, ' ');
    if (parts.size() != 3 || parts[0].empty() || parts[0][0] == '@' || parts[0][0] == '#')
      return false;
    if (!std::ranges::all_of(line, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
      return false;
    any = true;
  }
  return any;
}

// The token rclone's own browser sign-in gives, as it stores it.
Result<std::string> SignIn(const config::Config& config, const std::string& provider,
                           const std::function<void(const std::string&)>& on_url) {
  const runner::ToolStatus tool = DetectRclone(config);
  if (!tool.installed) return Err("rclone_missing", "rclone isn't installed.");
  Command command;
  command.argv = {tool.path,   "--config", RcloneConfigFile(config).string(),
                  "authorize", provider,   "--auth-no-open-browser"};
  command.unset_prefixes = {"RCLONE_"};
  command.timeout_s = kSignInTimeoutS;
  std::string seen;
  bool told = false;
  const auto run = runner::RunAndWait(command, [&](std::string_view chunk) {
    if (told) return;
    seen += chunk;
    constexpr std::string_view kStart = "http://127.0.0.1:53682/auth?state=";
    const std::size_t at = seen.find(kStart);
    if (at == std::string::npos) return;
    const std::size_t end = seen.find_first_of(" \r\n", at);
    if (end == std::string::npos) return;
    told = true;
    on_url(seen.substr(at, end - at));
  });
  if (!run) return std::unexpected(run.error());
  if (!told && run->output.contains("address already in use"))
    return Err("remote_sign_in", "Another sign-in is already waiting on this computer.");
  const std::size_t arrow = run->output.find("--->");
  const std::size_t start =
      arrow == std::string::npos ? std::string::npos : run->output.find('{', arrow);
  const std::size_t end = run->output.find("<---", start == std::string::npos ? 0 : start);
  const json token = run->exit_code != 0 || start == std::string::npos || end == std::string::npos
                         ? json()
                         : json::parse(run->output.substr(start, end - start), nullptr, false);
  if (!token.is_object() || core::JsonString(token, "access_token").empty())
    return Err("remote_sign_in", "The sign-in wasn't finished.");
  return token.dump();
}

// OneDrive's own drive, which rclone's config names.
Result<void> AddDrive(RemoteRequest& request) {
  const json token = json::parse(request.values["token"], nullptr, false);
  const auto answer = runner::RunCurlWithSecrets(
      {"-sS", "--proto", "=https", "--max-time", "20",
       "https://graph.microsoft.com/v1.0/me/drive?$select=id,driveType"},
      runner::CurlConfigLine("header",
                             "Authorization: Bearer " + core::JsonString(token, "access_token")));
  if (!answer) return std::unexpected(answer.error());
  const json drive = json::parse(answer->output, nullptr, false);
  const std::string id = drive.is_object() ? core::JsonString(drive, "id") : std::string();
  const std::string type = drive.is_object() ? core::JsonString(drive, "driveType") : std::string();
  if (id.empty() || type.empty())
    return Err("remote_sign_in", "That account has no OneDrive Mira can use.");
  request.values["drive_id"] = id;
  request.values["drive_type"] = type;
  return {};
}

}  // namespace

Result<void> Probe(const config::Config& config, const Remote& remote) {
  std::string file = (fs::temp_directory_path() / "mira-check-XXXXXX").string();
  const int fd = ::mkostemp(file.data(), O_CLOEXEC);
  if (fd < 0) return Err("remote_failed", "couldn't make a temporary file");
  ::close(fd);
  const bool bare = remote.root.ends_with(':') || remote.root.ends_with('/');
  const std::string target =
      std::format("{}{}mira-saves/check-{}", remote.root, bare ? "" : "/", Unique());
  auto sent = RunRclone(config, remote, {"copyto", file, target});
  std::error_code ec;
  fs::remove(file, ec);
  if (!sent) return std::unexpected(sent.error());
  if (auto gone = RunRclone(config, remote, {"deletefile", target}); !gone)
    return std::unexpected(gone.error());
  return {};
}

Result<RemoteInfo> Connect(const config::Config& config, RemoteRequest request,
                           const std::string& host_keys,
                           const std::function<void(const std::string& url)>& on_url) {
  // These come from the sign-in, never from a client.
  for (const char* key : {"token", "drive_id", "drive_type"})
    if (request.values.contains(key)) return Err("invalid_remote", "An unknown setting was given.");
  if (!host_keys.empty()) {
    if (request.provider != "sftp" || !HostKeysOk(host_keys) ||
        (request.slot != "primary" && request.slot != "mirror"))
      return Err("invalid_remote", "Those host keys aren't valid.");
    const fs::path file = Folder(config) / (KnownHostsPrefix(request.slot) + Unique());
    if (auto written = WriteFileAtomic(file, host_keys, "remote_config_failed", true, 0600);
        !written)
      return std::unexpected(written.error());
    request.values["known_hosts_file"] = file.string();
  }
  const auto drop = [&] {
    std::error_code ec;
    if (!host_keys.empty()) fs::remove(request.values["known_hosts_file"], ec);
  };
  auto connected = [&]() -> Result<RemoteInfo> {
    if (auto checked = CheckRequest(request); !checked) return std::unexpected(checked.error());
    if (!DetectRclone(config).installed)
      if (auto installed = InstallRclone(config); !installed)
        return std::unexpected(installed.error());
    if (SignsIn(request.provider)) {
      auto token = SignIn(config, request.provider, on_url);
      if (!token) return std::unexpected(token.error());
      request.values["token"] = *token;
      if (request.provider == "onedrive")
        if (auto drive = AddDrive(request); !drive) return std::unexpected(drive.error());
    }
    return SaveRemote(config, request, [&](const Remote& remote) { return Probe(config, remote); });
  }();
  if (!connected) {
    drop();
    return connected;
  }
  DropKnownHosts(config, request.slot,
                 host_keys.empty() ? fs::path() : fs::path(request.values["known_hosts_file"]));
  return connected;
}

Result<void> Disconnect(const config::Config& config, std::string_view slot) {
  if (auto gone = ForgetRemote(config, slot); !gone) return gone;
  DropKnownHosts(config, slot, {});
  return {};
}

Result<json> ScanHost(const std::string& host, const std::string& port) {
  if (!SftpHost(host)) return Err("invalid_remote", "The host name isn't valid.");
  if (!port.empty() && !ValidPort(port))
    return Err("invalid_remote", "The port must be 1 to 65535.");
  if (auto ready = system::RequireFeature("ssh", "Syncing over SSH"); !ready)
    return std::unexpected(ready.error());
  Command scan;
  scan.argv = {"ssh-keyscan", "-T", "10", "-p", port.empty() ? "22" : port, "--", host};
  scan.timeout_s = 40;
  const auto run = runner::RunAndWait(scan);
  if (!run) return std::unexpected(run.error());
  std::string keys;
  for (const std::string& line : strings::Split(run->output, '\n'))
    if (!line.starts_with('#') && HostKeysOk(line)) keys += line + "\n";
  if (keys.empty())
    return Err("remote_offline", std::format("No SSH server answered at {}.", host));
  std::string file = (fs::temp_directory_path() / "mira-host-XXXXXX").string();
  const int fd = ::mkostemp(file.data(), O_CLOEXEC);
  if (fd < 0) return Err("remote_failed", "couldn't make a temporary file");
  ::close(fd);
  Command print;
  print.argv = {"ssh-keygen", "-l", "-f", file};
  Result<runner::ExecResult> printed = Err("remote_failed", "couldn't read the server's keys");
  if (WriteFileAtomic(file, keys, "remote_failed", false, 0600))
    printed = runner::RunAndWait(print);
  std::error_code ec;
  fs::remove(file, ec);
  if (!printed || printed->exit_code != 0)
    return Err("remote_failed", "couldn't read the server's keys");
  json prints = json::array();
  for (const std::string& line : strings::Split(printed->output, '\n')) {
    // "256 SHA256:... host (ED25519)"
    const std::vector<std::string> parts = strings::Split(line, ' ');
    if (parts.size() >= 4) prints.push_back(std::format("{} {}", parts.back(), parts[1]));
  }
  return json{{"host_keys", keys}, {"fingerprints", std::move(prints)}};
}

Result<json> SshKey(const config::Config& config) {
  const fs::path key = Folder(config) / "saves-ssh-key";
  std::error_code ec;
  if (!fs::exists(key, ec)) {
    if (auto ready = system::RequireFeature("ssh", "Syncing over SSH"); !ready)
      return std::unexpected(ready.error());
    Command make;
    make.argv = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "mira", "-f", key.string()};
    const auto run = runner::RunAndWait(make);
    if (!run) return std::unexpected(run.error());
    if (run->exit_code != 0) return Err("remote_failed", "couldn't make an SSH key");
  }
  const auto text = files::ReadFile(key.string() + ".pub");
  if (!text) return Err("remote_failed", "couldn't read the SSH key's public half");
  return json{{"path", key.string()}, {"public", strings::Trim(*text)}};
}

}  // namespace mira::saves::remote
