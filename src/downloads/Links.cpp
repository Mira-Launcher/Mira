#include "downloads/Links.h"

#include <cctype>
#include <format>
#include <json.hpp>
#include <regex>
#include <sstream>

#include "core/Strings.h"
#include "runner/Curl.h"
#include "runner/Exec.h"

namespace mira::downloads {
namespace {

const std::regex kPixeldrainFile(R"(pixeldrain\.[a-z]+/(?:u|api/file)/([A-Za-z0-9_-]+))",
                                 std::regex::icase);
const std::regex kPixeldrainList(R"(pixeldrain\.[a-z]+/l/([A-Za-z0-9_-]+))", std::regex::icase);

struct Probe {
  int status = 0;
  std::string content_type;
  std::string disposition;
  std::int64_t size = 0;
};

// What the server says the address is, following redirects, without downloading it.
Result<Probe> Head(const std::string& url) {
  Command command;
  command.argv = {
      "curl",
      "-sIL",
      "--max-time",
      "20",
      "-o",
      "/dev/null",
      "-w",
      "%{http_code}\n%header{content-type}\n%header{content-disposition}\n%header{content-length}",
      "--",
      url};
  const auto result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0)
    return Err("link_unreachable", std::format("couldn't reach {}", url));
  std::istringstream lines(result->output);
  Probe probe;
  std::string size;
  std::string status;
  std::getline(lines, status);
  probe.status = std::atoi(status.c_str());
  std::getline(lines, probe.content_type);
  std::getline(lines, probe.disposition);
  std::getline(lines, size);
  probe.size = std::strtoll(size.c_str(), nullptr, 10);
  return probe;
}

Result<std::string> Page(const std::string& url) {
  Command command;
  // A page is small: an address that turns out to be a big file gives up rather than fetching it.
  command.argv = {"curl", "-sL", "--max-time", "20", "--max-filesize", "10M", "--", url};
  const auto result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0)
    return Err("link_unreachable", std::format("couldn't reach {}", url));
  return result->output;
}

Result<std::vector<LinkFile>> Pixeldrain(const std::string& link) {
  std::smatch match;
  if (std::regex_search(link, match, kPixeldrainList)) {
    const auto list =
        runner::CurlJson(std::format("https://pixeldrain.com/api/list/{}", match[1].str()));
    if (!list) return std::unexpected(list.error());
    std::vector<LinkFile> files;
    if (list->is_object() && list->contains("files") && (*list)["files"].is_array()) {
      for (const auto& file : (*list)["files"]) {
        files.push_back({.url = std::format("https://pixeldrain.com/api/file/{}?download",
                                            file.value("id", std::string())),
                         .name = file.value("name", std::string()),
                         .size = file.value("size", std::int64_t{0}),
                         .source = "Pixeldrain"});
      }
    }
    if (files.empty()) return Err("link_unreadable", "that Pixeldrain list has no files");
    return files;
  }
  std::regex_search(link, match, kPixeldrainFile);
  const std::string id = match[1].str();
  const auto info = runner::CurlJson(std::format("https://pixeldrain.com/api/file/{}/info", id));
  if (!info) return std::unexpected(info.error());
  if (!info->is_object() || !info->contains("name"))
    return Err("link_unreadable", "Pixeldrain doesn't know that file");
  return std::vector<LinkFile>{
      {.url = std::format("https://pixeldrain.com/api/file/{}?download", id),
       .name = info->value("name", std::string()),
       .size = info->value("size", std::int64_t{0}),
       .source = "Pixeldrain"}};
}

Result<std::vector<LinkFile>> MediaFire(const std::string& link) {
  // Some links answer with the file itself instead of a page.
  if (const auto probe = Head(link);
      probe && probe->status < 400 && probe->content_type.find("text/html") == std::string::npos)
    return std::vector<LinkFile>{{.url = link,
                                  .name = NameFromDisposition(probe->disposition),
                                  .size = probe->size,
                                  .source = "MediaFire"}};
  const auto page = Page(link);
  if (!page) return std::unexpected(page.error());
  const std::string address = MediaFireAddressIn(*page);
  if (address.empty())
    return Err("link_unreadable", "couldn't find the download on that MediaFire page");
  const auto file = Head(address);
  return std::vector<LinkFile>{{.url = address,
                                .name = file ? NameFromDisposition(file->disposition) : "",
                                .size = file ? file->size : 0,
                                .source = "MediaFire"}};
}

bool IsPixeldrain(const std::string& link) {
  return std::regex_search(link, kPixeldrainFile) || std::regex_search(link, kPixeldrainList);
}

bool IsMediaFire(const std::string& link) {
  return strings::ToLower(link).find("mediafire.com/") != std::string::npos;
}

}  // namespace

bool IsWebLink(const std::string& link) {
  const std::string lower = strings::ToLower(link);
  return lower.starts_with("http://") || lower.starts_with("https://") ||
         lower.starts_with("magnet:");
}

LinkKind Classify(const std::string& link) {
  const std::string lower = strings::ToLower(link);
  if (lower.starts_with("magnet:") || lower.ends_with(".torrent")) return LinkKind::Torrent;
  if (IsPixeldrain(link) || IsMediaFire(link)) return LinkKind::Host;
  return LinkKind::Direct;
}

std::string MediaFireAddressIn(const std::string& page) {
  static const std::regex kAddress(R"(href="(https?://download[^"]*mediafire\.com/[^"]+)\")");
  std::smatch match;
  return std::regex_search(page, match, kAddress) ? match[1].str() : std::string();
}

namespace {

std::string PercentDecoded(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(text[i + 1]) != 0 &&
        std::isxdigit(text[i + 2]) != 0) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

}  // namespace

std::string NameFromDisposition(const std::string& header) {
  static const std::regex kName(R"re(filename(\*?)=(?:UTF-8'')?"?([^";\r\n]+)"?)re",
                                std::regex::icase);
  std::smatch match;
  if (!std::regex_search(header, match, kName)) return {};
  return match[1].length() > 0 ? PercentDecoded(match[2].str()) : match[2].str();
}

std::string CleanFileName(const std::string& name, const std::string& url) {
  std::string out = name;
  if (out.empty()) {
    out = PercentDecoded(url.substr(0, url.find_first_of("?#")));
  }
  const auto slash = out.find_last_of("/\\");
  if (slash != std::string::npos) out.erase(0, slash + 1);
  std::erase(out, '\0');
  const auto first = out.find_first_not_of(". ");
  return first == std::string::npos ? std::string("download") : out.substr(first);
}

namespace {

Result<std::vector<LinkFile>> ResolveRaw(const std::string& link) {
  if (IsPixeldrain(link)) return Pixeldrain(link);
  if (IsMediaFire(link)) return MediaFire(link);
  const auto probe = Head(link);
  if (!probe) return std::unexpected(probe.error());
  if (probe->status == 404 || probe->status == 410)
    return Err("link_not_found", std::format("{} isn't there any more", link));
  // A server that refuses a HEAD request may still serve the file, so only a page is turned away.
  if (probe->status < 400 && probe->content_type.find("text/html") != std::string::npos)
    return Err("link_is_a_page", std::format("{} is a web page, not a file", link),
               "Use the file's own download address, or set up a download service.");
  return std::vector<LinkFile>{{.url = link,
                                .name = NameFromDisposition(probe->disposition),
                                .size = probe->size,
                                .source = "Direct"}};
}

}  // namespace

Result<std::vector<LinkFile>> Resolve(const std::string& link) {
  auto files = ResolveRaw(link);
  if (files) {
    for (LinkFile& file : *files) file.name = CleanFileName(file.name, file.url);
  }
  return files;
}

}  // namespace mira::downloads
