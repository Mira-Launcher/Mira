#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "core/Result.h"

// Autodesk's desktop apps (AutoCAD, Inventor, 3ds Max, Revit), installed from the Create Installer
// the user downloads from their Autodesk account. It builds an install image in the prefix's
// Downloads folder, whose Setup.exe then installs the app.
namespace mira::launchers::autodesk {

// Whether `file` is a Create Installer for the product `code`
// (Create_Installer_ACD_2027_1_English_WIN64.exe).
bool IsCreateInstaller(const std::filesystem::path& file, std::string_view code);

// The newest install image's Setup.exe in `prefix`, written after `since`.
std::optional<std::filesystem::path> FindImage(const std::filesystem::path& prefix,
                                               std::filesystem::file_time_type since);

// Whether the Create Installer has finished the image, by its log, which ForgetImageBuilt clears
// beforehand.
bool ImageBuilt(const std::filesystem::path& prefix);
void ForgetImageBuilt(const std::filesystem::path& prefix);

// Readies `setup` (an image's Setup.exe) to run under Wine.
Result<void> PrepareImage(const std::filesystem::path& setup);

}  // namespace mira::launchers::autodesk
