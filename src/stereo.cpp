// SPDX-License-Identifier: GPL-3.0-only
#include "stereo.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace xr {
std::optional<bool> decodeStereoState(std::string_view text) {
  std::istringstream stream{std::string(text)};
  std::string line, brand, model, supported, enabled;
  while (std::getline(stream, line)) {
    auto split = line.find('=');
    if (split == std::string::npos)
      continue;
    auto key = line.substr(0, split), value = line.substr(split + 1);
    if (key == "connected_device_brand")
      brand = value;
    if (key == "connected_device_model")
      model = value;
    if (key == "sbs_mode_supported")
      supported = value;
    if (key == "sbs_mode_enabled")
      enabled = value;
  }
  if ((brand != "XREAL" && brand != "Nreal") || model != "Air" ||
      supported != "true")
    return std::nullopt;
  if (enabled == "true")
    return true;
  if (enabled == "false")
    return false;
  return std::nullopt;
}

std::optional<bool> readStereoState(const std::string &path, uint64_t now) {
  struct stat st{};
  if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size > 16384)
    return std::nullopt;
  const uint64_t stamp =
      uint64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
  if (stamp > now || now - stamp > 3000)
    return std::nullopt;
  std::ifstream file(path);
  std::string state{std::istreambuf_iterator<char>(file), {}};
  return decodeStereoState(state);
}

StereoLayout stereoLayout(const std::string &mode, bool preview, int width,
                          int height, int outputWidth, int outputHeight,
                          int scale, int transform,
                          std::optional<bool> hardwareSbs) {
  if (preview)
    return mode == "sbs" ? StereoLayout::sbs : StereoLayout::mono;
  if (scale != 1 || transform != 0 || width != outputWidth ||
      height != outputHeight || height != 1080 || !hardwareSbs)
    return StereoLayout::waiting;
  if (*hardwareSbs && width == 3840 && mode != "mono")
    return StereoLayout::sbs;
  if (!*hardwareSbs && width == 1920 && mode != "sbs")
    return StereoLayout::mono;
  return StereoLayout::waiting;
}
} // namespace xr
