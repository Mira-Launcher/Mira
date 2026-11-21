#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mira::library {

// A Windows program's own name from its version resource: ProductName, else FileDescription. Empty
// when it has none or isn't a Windows program.
std::string PeProductName(const std::filesystem::path& exe);

// The value of `key` in a VS_VERSIONINFO block's string table, as UTF-8; empty when absent.
std::string VersionString(std::string_view block, std::string_view key);

}  // namespace mira::library
