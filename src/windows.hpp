// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wayland-client.h>

namespace xr {
struct WindowImage {
  std::string identifier, address, appId, title;
  unsigned texture = 0;
  int width = 0, height = 0;
  bool ready = false, swapRB = false, opaque = true;
};

// Each frame is owned until image-copy ready/failed. wl_buffer.release is not
// part of that protocol's capture completion contract.
class Windows {
  struct Impl;
  std::unique_ptr<Impl> p;

public:
  explicit Windows(wl_display *display);
  ~Windows();
  bool supported() const;
  std::vector<WindowImage> images() const;
  void update(const std::vector<std::string> &addresses, double rate);
  uint64_t frames() const;
  uint64_t failures() const;
};
std::vector<WindowImage> listWindows();
} // namespace xr
