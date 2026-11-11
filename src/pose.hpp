// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "math.hpp"
#include <cstdint>
#include <span>
#include <string>
namespace xr {
struct Pose {
  Quat orientation;
  uint64_t timestamp = 0;
  uint64_t referenceGeneration = 0;
  bool connected = false, valid = false;
  std::string calibration = "UNKNOWN", status = "unavailable";
};
class PoseProvider {
public:
  virtual ~PoseProvider() = default;
  virtual Pose sample() = 0;
};
// Source-verified packed v5 ABI. No C struct casts or pthread ABI dependencies.
namespace breezy_v5 {
constexpr size_t size = 186, timestamp = 113, orientation = 121, parity = 185;
Pose decode(std::span<const uint8_t> bytes, uint64_t now, uint64_t timeout);
} // namespace breezy_v5
uint64_t epochMs();

// Owns the rendered orientation and its neutral pose. Provider timestamps
// still govern freshness independently of the filter.
class HeadTracking {
  Quat reference_, previous_, filtered_;
  uint64_t timestamp_ = 0, generation_ = 0, timeout_;
  double speed_ = 0;
  bool initialized_ = false, interrupted_ = false;

  void resetFilter(const Pose &);

public:
  enum class Change { none, acquired, resumed, referenceReset };
  explicit HeadTracking(uint64_t timeout) : timeout_(timeout) {}
  Change update(const Pose &);
  bool recenter(const Pose &);
  Quat orientation() const { return previous_; }
  Quat rotation() const {
    return (reference_.inverse() * filtered_).normalized();
  }
};

class BreezyPose final : public PoseProvider {
  std::string path_, statePath_;
  uint64_t timeout_;

public:
  explicit BreezyPose(std::string path = "/dev/shm/breezy_desktop_imu",
                      uint64_t timeout = 250,
                      std::string statePath = "/dev/shm/xr_driver_state")
      : path_(std::move(path)), statePath_(std::move(statePath)),
        timeout_(timeout) {}
  Pose sample() override;
};
// XRLinuxDriver's supported OpenTrack source: 6 native little-endian doubles
// (driver-normalized positions, degrees yaw/pitch/roll), then uint32 counter.
// The protocol has no sender timestamp/version; we validate its exact 52-byte
// shape and use kernel receive timestamps. Bind to loopback only.
Pose decodeOpenTrack(std::span<const uint8_t> bytes, uint64_t receiveMs);
// Tracks the sender and counter independently of socket I/O. Preserve a
// reference change even when newer packets replace the reset packet in a drain.
class OpenTrackSequence {
  uint32_t frame_ = 0;
  uint16_t sender_ = 0;
  uint64_t generation_ = 0, timestamp_ = 0, timeout_;
  bool initialized_ = false;

public:
  explicit OpenTrackSequence(uint64_t timeout) : timeout_(timeout) {}
  bool accept(uint32_t frame, uint16_t sender, Pose &pose);
};

class OpenTrackPose final : public PoseProvider {
  int fd_ = -1;
  uint64_t timeout_;
  Pose last_;
  OpenTrackSequence sequence_;

public:
  explicit OpenTrackPose(int port = 4242, uint64_t timeout = 250);
  ~OpenTrackPose() override;
  Pose sample() override;
};
} // namespace xr
