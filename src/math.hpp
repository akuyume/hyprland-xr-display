// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace xr {
constexpr double pi = 3.14159265358979323846;
constexpr double rad(double d) { return d * pi / 180.; }
struct Vec {
  double x, y, z;
};
struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
  Quat normalized() const {
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(n) || n < 1e-8)
      throw std::runtime_error("invalid quaternion");
    return {x / n, y / n, z / n, w / n};
  }
  Quat inverse() const { return {-x, -y, -z, w}; }
  Quat operator*(Quat b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z};
  }
  Vec rotate(Vec v) const {
    auto r = (*this) * Quat{v.x, v.y, v.z, 0} * inverse();
    return {r.x, r.y, r.z};
  }
  static Quat axis(Vec a, double angle) {
    double s = std::sin(angle / 2);
    return {a.x * s, a.y * s, a.z * s, std::cos(angle / 2)};
  }
};
// Rotation-vector signs avoid Euler subtraction and its wrap discontinuities.
inline Quat axisPolicy(Quat q, bool iy, bool ip, bool ir, bool roll) {
  if (q.w < 0)
    q = {-q.x, -q.y, -q.z, -q.w};
  double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (s < 1e-9)
    return {};
  double a = 2 * std::atan2(s, q.w);
  auto adjusted = Quat::axis({q.x / s * (ip ? -1 : 1), q.y / s * (iy ? -1 : 1),
                              q.z / s * (roll && ir ? -1 : 1)},
                             a);
  if (roll)
    return adjusted;
  // Reconstruct a level camera from its forward ray. Zeroing the quaternion's
  // roll component would also change where a combined rotation is looking.
  auto forward = adjusted.rotate({0, 0, -1});
  auto horizontal = std::hypot(forward.x, forward.z);
  auto yaw = horizontal > 1e-9 ? std::atan2(-forward.x, -forward.z) : 0.;
  auto pitch = std::atan2(forward.y, horizontal);
  return Quat::axis({0, 1, 0}, yaw) * Quat::axis({1, 0, 0}, pitch);
}
inline std::array<double, 3> euler(Quat q) {
  // Diagnostics only: yaw(Y), pitch(X), roll(Z), intrinsic Y-X-Z.
  auto f = q.rotate({0, 0, -1});
  auto u = q.rotate({0, 1, 0});
  return {std::atan2(-f.x, -f.z) * 180 / pi,
          std::asin(std::clamp(f.y, -1., 1.)) * 180 / pi,
          std::atan2(-u.x * std::cos(std::atan2(-f.x, -f.z)) +
                         u.z * std::sin(std::atan2(-f.x, -f.z)),
                     u.y) *
              180 / pi};
}
} // namespace xr
