// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xr {
enum class StereoLayout { waiting, mono, sbs };

std::optional<bool> decodeStereoState(std::string_view state);
std::optional<bool> readStereoState(const std::string &path, uint64_t now);
StereoLayout stereoLayout(const std::string &mode, bool preview, int width,
                          int height, int outputWidth, int outputHeight,
                          int scale, int transform,
                          std::optional<bool> hardwareSbs);
inline double eyeOffset(int eye, double ipdMm) {
  // Air Lab's parallel cameras, in meters along the current head-right axis.
  return (eye == 0 ? -1 : 1) * ipdMm / 2000.;
}
} // namespace xr
