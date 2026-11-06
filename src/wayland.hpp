// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "config.hpp"
#include "pose.hpp"
#include <memory>
#include <string>
namespace xr {
// Backend boundary: textures and capture lifetime remain outside pose/render
// math.
class CaptureBackend {
public:
  virtual ~CaptureBackend() = default;
  virtual void request() = 0;
  virtual unsigned texture() const = 0;
};
class Display {
  struct Impl;
  std::unique_ptr<Impl> p;

public:
  Display(const Config &, const std::string &source, bool preview, bool flat);
  ~Display();
  void dispatch(int timeoutMs);
  void render(Quat delta);
  void maintain();
  std::string statistics() const;
};
} // namespace xr
