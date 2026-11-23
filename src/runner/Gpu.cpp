#include "runner/Gpu.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

#include "core/Files.h"
#include "core/Paths.h"
#include "core/Strings.h"

namespace mira::runner {
namespace {

namespace fs = std::filesystem;

struct Gpu {
  std::string vendor;  // "10de"
  std::string device;  // "2860"
  bool boot = false;   // the one the PC started its display on: a laptop's integrated GPU
  bool integrated = false;
};

// Intel's integrated GPU is always at PCI 00:02.0; an AMD one (an APU) has little VRAM of its own.
bool IsIntegrated(const fs::path& device, std::string_view vendor) {
  std::error_code ec;
  if (vendor == "8086") return fs::canonical(device, ec).filename().string().ends_with(":00:02.0");
  if (vendor != "1002") return false;
  const std::string vram =
      strings::Trim(files::ReadFile(device / "mem_info_vram_total").value_or(""));
  std::uint64_t bytes = 0;
  std::from_chars(vram.data(), vram.data() + vram.size(), bytes);
  return bytes > 0 && bytes < (std::uint64_t{2} << 30);
}

std::string HexId(const fs::path& file) {
  std::string text = strings::Trim(files::ReadFile(file).value_or(""));
  return text.starts_with("0x") ? text.substr(2) : text;
}

std::vector<Gpu> Gpus(const fs::path& drm) {
  std::vector<Gpu> gpus;
  for (const fs::path& card : paths::ListDir(drm)) {
    const std::string name = card.filename().string();
    // card0, card1: not their outputs (card0-HDMI-A-1) or render nodes.
    if (!name.starts_with("card") || name.find('-') != std::string::npos) continue;
    Gpu gpu{
        .vendor = HexId(card / "device" / "vendor"),
        .device = HexId(card / "device" / "device"),
        .boot = strings::Trim(files::ReadFile(card / "device" / "boot_vga").value_or("")) == "1"};
    gpu.integrated = IsIntegrated(card / "device", gpu.vendor);
    if (!gpu.vendor.empty() && !gpu.device.empty()) gpus.push_back(std::move(gpu));
  }
  return gpus;
}

}  // namespace

std::map<std::string, std::string> GpuEnv(std::string_view choice, const fs::path& drm) {
  if (choice != "dedicated" && choice != "integrated") return {};
  const std::vector<Gpu> gpus = Gpus(drm);
  if (gpus.size() < 2) return {};
  const auto pick = std::ranges::find_if(
      gpus, [&](const Gpu& gpu) { return gpu.boot == (choice == "integrated"); });
  // An NVIDIA card is never the integrated one, nor is the GPU the PC booted on a dedicated one.
  if (pick == gpus.end() || (choice == "integrated" && pick->vendor == "10de")) return {};
  std::map<std::string, std::string> env;
  if (pick->vendor == "10de") {
    env["__NV_PRIME_RENDER_OFFLOAD"] = "1";
    env["__GLX_VENDOR_LIBRARY_NAME"] = "nvidia";
    env["__VK_LAYER_NV_optimus"] = "NVIDIA_only";
    return env;
  }
  // Mesa's Vulkan layer takes "vendor:device!" as the only device to offer.
  env["MESA_VK_DEVICE_SELECT"] = pick->vendor + ":" + pick->device + "!";
  env["DRI_PRIME"] = choice == "dedicated" ? "1" : "0";
  if (std::ranges::any_of(gpus, [](const Gpu& gpu) { return gpu.vendor == "10de"; })) {
    env["__VK_LAYER_NV_optimus"] = "non_NVIDIA_only";
    env["__GLX_VENDOR_LIBRARY_NAME"] = "mesa";
  }
  return env;
}

GameGpu GameGpuFor(std::string_view choice, const fs::path& drm) {
  std::vector<Gpu> gpus = Gpus(drm);
  // The kind asked for, when the PC has it; else any could run the game.
  if (choice == "dedicated" || choice == "integrated") {
    const bool integrated = choice == "integrated";
    std::vector<Gpu> picked;
    std::ranges::copy_if(gpus, std::back_inserter(picked),
                         [&](const Gpu& gpu) { return gpu.integrated == integrated; });
    if (!picked.empty()) gpus = std::move(picked);
  }
  GameGpu game;
  game.integrated =
      !gpus.empty() && std::ranges::all_of(gpus, [](const Gpu& gpu) { return gpu.integrated; });
  game.nvidia = std::ranges::any_of(gpus, [](const Gpu& gpu) { return gpu.vendor == "10de"; });
  // RDNA 4 (Radeon RX 9000) device IDs are 0x75xx.
  game.rdna4 = std::ranges::any_of(
      gpus, [](const Gpu& gpu) { return gpu.vendor == "1002" && gpu.device.starts_with("75"); });
  return game;
}

}  // namespace mira::runner
