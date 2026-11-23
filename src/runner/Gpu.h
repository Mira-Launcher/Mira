#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace mira::runner {

// The env that runs a game on a PC's "dedicated" or "integrated" GPU (launch.gpu): NVIDIA's PRIME
// offload for an NVIDIA card, else Mesa's device selection. Empty for "" (the system's choice) or a
// PC with one GPU. `drm` is where the GPUs are listed.
std::map<std::string, std::string> GpuEnv(std::string_view choice,
                                          const std::filesystem::path& drm = "/sys/class/drm");

// What Mira knows of the GPU a game runs on (launch.gpu), for the flags that depend on it.
struct GameGpu {
  bool integrated = false;  // only an integrated GPU: too slow for ray tracing
  bool nvidia = false;      // DLSS
  bool rdna4 = false;       // FSR 4
};
GameGpu GameGpuFor(std::string_view choice, const std::filesystem::path& drm = "/sys/class/drm");

}  // namespace mira::runner
