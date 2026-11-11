// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "config.hpp"
#include "math.hpp"
#include "windows.hpp"
#include <memory>
#include <string>
#include <vector>

namespace xr {
struct Panel {
  WindowImage image;
  Vec center;
  Quat rotation;
  double halfWidth = 0, halfHeight = 0;
};
class Scene {
  struct Impl;
  std::unique_ptr<Impl> p;

public:
  explicit Scene(std::string source);
  ~Scene();
  std::vector<std::string> addresses() const;
  std::vector<Panel> panels(const std::vector<WindowImage> &,
                            const Config &) const;
  void refreshSettings();
  std::string readyGeneration(const std::vector<WindowImage> &) const;
  std::string status() const;
};
} // namespace xr
