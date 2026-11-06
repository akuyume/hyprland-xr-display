// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "config.hpp"
#include <nlohmann/json.hpp>
namespace xr {
std::string hypr(const std::vector<std::string> &args);
nlohmann::json monitors();
class VirtualOutput {
  bool owned_ = false;
  int id_ = -1;

public:
  std::string name;
  explicit VirtualOutput(const Config &c);
  ~VirtualOutput();
  VirtualOutput(const VirtualOutput &) = delete;
  VirtualOutput &operator=(const VirtualOutput &) = delete;
};
} // namespace xr
