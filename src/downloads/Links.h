#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Result.h"

namespace mira::downloads {

enum class LinkKind { Direct, Host, Torrent };

LinkKind Classify(const std::string& link);

// A file a link stands for.
struct LinkFile {
  std::string url;
  std::string name;  // empty when the server decides it
  std::int64_t size = 0;
  std::string source;  // "Direct", "Pixeldrain", ...
};

// Asks a known host for its real download address. `page` is a host page's HTML, for hosts that
// keep the address in it; the pure parsing, so it can be tested.
std::string MediaFireAddressIn(const std::string& page);
// Filename from a Content-Disposition header, or empty.
std::string NameFromDisposition(const std::string& header);

// The files a direct or host link stands for. A link that leads to a web page Mira can't read
// fails with link_is_a_page.
Result<std::vector<LinkFile>> Resolve(const std::string& link);

}  // namespace mira::downloads
