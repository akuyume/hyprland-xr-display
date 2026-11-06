// SPDX-License-Identifier: GPL-3.0-only
#include "config.hpp"
#include <cmath>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>
namespace xr {
static std::string trim(std::string v) {
  auto a = v.find_first_not_of(" \t\r\n");
  if (a == v.npos)
    return {};
  return v.substr(a, v.find_last_not_of(" \t\r\n") - a + 1);
}
void Config::load(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot open config: " + path);
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    ++n;
    line = trim(line.substr(0, line.find('#')));
    if (line.empty())
      continue;
    auto eq = line.find('=');
    if (eq == line.npos)
      throw std::runtime_error("config line " + std::to_string(n) +
                               ": expected key=value");
    auto k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    auto number = [&] {
      size_t end;
      double d = std::stod(v, &end);
      if (end != v.size() || !std::isfinite(d))
        throw std::runtime_error("invalid number for " + k);
      return d;
    };
    auto boolean = [&] {
      if (v != "true" && v != "false")
        throw std::runtime_error("invalid boolean for " + k);
      return v == "true";
    };
#define STR(x)                                                                 \
  if (k == #x) {                                                               \
    x = v;                                                                     \
    continue;                                                                  \
  }
#define NUM(x)                                                                 \
  if (k == #x) {                                                               \
    x = number();                                                              \
    continue;                                                                  \
  }
#define INT(x)                                                                 \
  if (k == #x) {                                                               \
    double d = number();                                                       \
    if (d < std::numeric_limits<int>::min() ||                                 \
        d > std::numeric_limits<int>::max() || std::floor(d) != d)             \
      throw std::runtime_error("invalid integer for " + k);                    \
    x = int(d);                                                                \
    continue;                                                                  \
  }
#define BOOL(x)                                                                \
  if (k == #x) {                                                               \
    x = boolean();                                                             \
    continue;                                                                  \
  }
    STR(target_output_make)
    STR(target_output_model)
    STR(target_output_name_override)
    STR(virtual_output_name) STR(virtual_resolution) STR(capture_backend)
        STR(pose_backend) STR(pose_path) STR(drm_device)
            NUM(virtual_refresh_rate) NUM(virtual_scale) NUM(display_distance)
                NUM(display_size) NUM(horizontal_fov) NUM(vertical_fov)
                    NUM(yaw_offset) NUM(pitch_offset) NUM(render_refresh_rate)
                        INT(opentrack_port) INT(pose_timeout_ms)
                            BOOL(roll_enabled) BOOL(invert_yaw)
                                BOOL(invert_pitch) BOOL(invert_roll)
#undef STR
#undef NUM
#undef BOOL
#undef INT
                                    throw std::runtime_error(
                                        "unknown config key: " + k);
  }
  validate();
}
void Config::validate() const {
  if (!std::regex_match(virtual_output_name, std::regex("[A-Za-z0-9_-]{1,64}")))
    throw std::runtime_error("invalid virtual output name");
  std::smatch m;
  if (!std::regex_match(virtual_resolution, m,
                        std::regex("([0-9]{3,4})x([0-9]{3,4})")) ||
      std::stoi(m[1]) > 8192 || std::stoi(m[2]) > 8192)
    throw std::runtime_error("invalid virtual resolution");
  if (virtual_scale != 1)
    throw std::runtime_error(
        "MVP requires virtual_scale=1 to preserve cursor scaling");
  if (display_distance < 0.1 || display_distance > 100 || display_size < 1 ||
      display_size > 160 || horizontal_fov < 5 || horizontal_fov > 170 ||
      vertical_fov < 5 || vertical_fov > 170 || render_refresh_rate < 1 ||
      render_refresh_rate > 240 || virtual_refresh_rate < 1 ||
      virtual_refresh_rate > 240 || pose_timeout_ms < 10 ||
      pose_timeout_ms > 5000)
    throw std::runtime_error("configuration value out of range");
  if (pose_backend != "breezy" && pose_backend != "opentrack")
    throw std::runtime_error("pose_backend must be breezy or opentrack");
  if (opentrack_port < 1024 || opentrack_port > 65535)
    throw std::runtime_error("invalid opentrack_port");
  if (capture_backend != "auto" && capture_backend != "dmabuf" &&
      capture_backend != "shm")
    throw std::runtime_error("capture_backend must be auto, dmabuf, or shm");
}
} // namespace xr
