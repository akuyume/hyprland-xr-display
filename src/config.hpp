// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
namespace xr {
struct Config {
  std::string target_output_make = "Nreal", target_output_model = "Air",
              target_output_name_override;
  std::string virtual_output_name = "XR-VIRTUAL",
              virtual_resolution = "1920x1080", capture_backend = "auto";
  std::string pose_path = "/dev/shm/breezy_desktop_imu",
              drm_device = "/dev/dri/renderD128";
  std::string stereo_mode = "auto",
              driver_state_path = "/dev/shm/xr_driver_state";
  double ipd_mm = 64;
  std::string view_mode = "desktop";
  double virtual_refresh_rate = 60, virtual_scale = 1, display_distance = 2,
         display_size = 32;
  double horizontal_fov = 40, vertical_fov = 23, yaw_offset = 0,
         pitch_offset = 0, render_refresh_rate = 60;
  bool roll_enabled = true, invert_yaw = false, invert_pitch = false,
       invert_roll = false;
  int pose_timeout_ms = 250;
  std::string pose_backend = "opentrack";
  int opentrack_port = 4242;
  void load(const std::string &path);
  void validate() const;
};
} // namespace xr
