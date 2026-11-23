// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The launcher's run entry points (`Inspect` and its zip reader). Internal to librbguest.so: the definitions
// live in `launcher.cpp`. The in-process runtime (`runtime_boot.cpp`) no longer runs `Inspect`: it boots a
// `GuestProcess` of its own, with no per-game profile.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ANativeWindow;

namespace rb::launcher {

/// What a run needs to draw: the launcher's window (null for a headless run — the scripted lanes, or no
/// screen), and how to turn an image file into a real GL texture on the context current on this thread.
/// `upload` is the Java half's BitmapFactory + GLUtils.texImage2D, the same calls the game's own
/// textureLoaded makes; it returns the GL name and the bitmap's size.
struct RenderHooks {
  ANativeWindow* window = nullptr;
  std::function<bool(const std::vector<std::uint8_t>& encoded, std::uint32_t& id, std::uint32_t& w,
                     std::uint32_t& h, std::string* why)>
      upload;
};

/// One entry of a zip held in memory, by exact name (see the definition in `launcher.cpp`). `name` empty
/// means the entry with the lowest local offset. `nullopt` with `why` set when it cannot be served.
std::optional<std::vector<std::uint8_t>> ZipEntry(const std::vector<std::uint8_t>& zip,
                                                  const std::string& name, std::string* why);

/// Runs a guest library through the engine and returns the report, one line per `\n`; every line is also
/// logged live under RBLAUNCH. `tiers`: 0 = interpreter then JIT, 1 = interpreter only, 2 = JIT only.
std::string Inspect(const std::vector<std::uint8_t>& app_image,
                    const std::vector<std::uint8_t>& libm_image,
                    const std::vector<std::uint8_t>& libc_image,
                    const std::vector<std::uint8_t>& libz_image,
                    const std::vector<std::uint8_t>& libstdcxx_image, const std::string& label,
                    int tiers, const std::string& apk_path, int surface_width,
                    int surface_height, const RenderHooks& render);

}  // namespace rb::launcher
