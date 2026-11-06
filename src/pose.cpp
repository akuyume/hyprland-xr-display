// SPDX-License-Identifier: GPL-3.0-only
#include "pose.hpp"
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
namespace xr {
uint64_t epochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
template <class T> static T unpack(std::span<const uint8_t> b, size_t at) {
  T v;
  std::memcpy(&v, b.data() + at, sizeof v);
  return v;
}
Pose breezy_v5::decode(std::span<const uint8_t> b, uint64_t now,
                       uint64_t timeout) {
  Pose p;
  if constexpr (std::endian::native != std::endian::little) {
    p.status = "unsupported host byte order";
    return p;
  }
  if (b.size() != size) {
    p.status = "incompatible pose record size (expected 186 bytes/v5)";
    return p;
  }
  if (b[0] != 5) {
    p.status = "unsupported pose ABI " + std::to_string(b[0]) + " (expected 5)";
    return p;
  }
  if (b[1] != 1) {
    p.status = "driver external stream disabled/disconnected";
    return p;
  }
  uint8_t check = 0;
  for (size_t i = timestamp; i < parity; ++i)
    check ^= b[i];
  if (check != b[parity]) {
    p.status = "inconsistent pose record";
    return p;
  }
  p.timestamp = unpack<uint64_t>(b, timestamp);
  if (p.timestamp > now + 50 ||
      (now > p.timestamp && now - p.timestamp > timeout)) {
    p.status = "pose timeout";
    return p;
  }
  p.connected = true;
  float x = unpack<float>(b, orientation),
        y = unpack<float>(b, orientation + 4),
        z = unpack<float>(b, orientation + 8),
        w = unpack<float>(b, orientation + 12);
  double n = x * x + y * y + z * z + w * w;
  if (!std::isfinite(n) || n < 0.8 || n > 1.2) {
    p.status = "invalid pose quaternion";
    return p;
  }
  // Driver NWU -> OpenGL EUS, same basis mapping documented by Breezy.
  p.orientation = Quat{-y, z, -x, w}.normalized();
  // All 16 reset values identify calibration/reset, not merely identity q.
  bool reset = true;
  for (int i = 0; i < 16; ++i) {
    float want = (i == 3 || i == 7 || i == 11) ? 1.f : 0.f;
    reset &= unpack<float>(b, orientation + i * 4) == want;
  }
  if (reset) {
    p.status = "driver pose reset/calibrating";
    return p;
  }
  p.valid = true;
  p.status = "tracking (Breezy ABI v5)";
  return p;
}
Pose BreezyPose::sample() {
  Pose p;
  int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    p.status = "pose stream unavailable; enable xr_driver_cli --breezy-desktop "
               "(requires driver productivity feature)";
    return p;
  }
  struct stat st{};
  std::array<uint8_t, breezy_v5::size> a{}, b{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != long(a.size())) {
    close(fd);
    p.status = "incompatible pose record size";
    return p;
  }
  // Reopen on each sample to survive replacement and avoid mmap SIGBUS on
  // truncation.
  bool stable = false;
  for (int i = 0; i < 3; ++i) {
    if (pread(fd, a.data(), a.size(), 0) != long(a.size()) ||
        pread(fd, b.data(), b.size(), 0) != long(b.size()))
      break;
    if (a == b) {
      stable = true;
      break;
    }
  }
  close(fd);
  if (!stable) {
    p.status = "inconsistent pose record";
    return p;
  }
  p = breezy_v5::decode(a, epochMs(), timeout_);
  // Small state file is the published state interface, never driver.log.
  std::ifstream state(statePath_);
  std::string line;
  while (std::getline(state, line))
    if (line.starts_with("calibration_state="))
      p.calibration = line.substr(18);
  if (p.calibration != "UNKNOWN" && p.calibration != "CALIBRATED") {
    p.valid = false;
    p.status = "calibration: " + p.calibration;
  }
  return p;
}

Pose decodeOpenTrack(std::span<const uint8_t> b, uint64_t received) {
  Pose p;
  if constexpr (std::endian::native != std::endian::little) {
    p.status = "unsupported OpenTrack byte order";
    return p;
  }
  if (b.size() != 52) {
    p.status =
        "incompatible OpenTrack packet (expected XRLinuxDriver 52-byte format)";
    return p;
  }
  double angles[3];
  for (int i = 0; i < 3; ++i) {
    angles[i] = unpack<double>(b, 24 + i * 8);
    if (!std::isfinite(angles[i]) || std::abs(angles[i]) > 360) {
      p.status = "invalid OpenTrack angle";
      return p;
    }
  }
  // XRLinuxDriver serializes NWU intrinsic Z-Y-X Euler angles.
  auto nwu = Quat::axis({0, 0, 1}, rad(angles[0])) *
             Quat::axis({0, 1, 0}, rad(angles[1])) *
             Quat::axis({1, 0, 0}, rad(angles[2]));
  p.orientation = Quat{-nwu.y, nwu.z, -nwu.x, nwu.w}.normalized();
  p.timestamp = received;
  p.connected = p.valid = true;
  p.calibration = "CALIBRATED";
  p.status =
      "tracking (XRLinuxDriver OpenTrack 52-byte UDP; receive timestamp)";
  return p;
}
OpenTrackPose::OpenTrackPose(int port, uint64_t timeout) : timeout_(timeout) {
  if (port < 1024 || port > 65535)
    throw std::runtime_error("invalid OpenTrack UDP port");
  fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0)
    throw std::runtime_error("OpenTrack socket failed");
  int on = 1;
  setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on);
  int size = 8192;
  setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd_, (sockaddr *)&addr, sizeof addr)) {
    close(fd_);
    fd_ = -1;
    throw std::runtime_error(
        "OpenTrack loopback port unavailable (already in use?)");
  }
  last_.status = "waiting for XRLinuxDriver OpenTrack UDP; enable "
                 "xr_driver_cli --opentrack-app";
}
OpenTrackPose::~OpenTrackPose() {
  if (fd_ >= 0)
    close(fd_);
}
Pose OpenTrackPose::sample() {
  // Drain all pending packets, retaining only the latest. Bounded socket queue.
  for (int i = 0; i < 256; ++i) {
    std::array<uint8_t, 53> b{};
    std::array<char, CMSG_SPACE(sizeof(timespec))> control{};
    iovec io{b.data(), b.size()};
    sockaddr_in sender{};
    msghdr msg{};
    msg.msg_name = &sender;
    msg.msg_namelen = sizeof sender;
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (sender.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
      continue;
    uint64_t received = epochMs();
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
        timespec t{};
        memcpy(&t, CMSG_DATA(c), sizeof t);
        received = uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
      }
    auto p = decodeOpenTrack(std::span(b).first(size_t(n)), received);
    if (p.valid) {
      uint32_t seq = unpack<uint32_t>(b, 48);
      if (haveSequence_ && seq == sequence_)
        continue;
      sequence_ = seq;
      haveSequence_ = true;
      last_ = p;
    } else if (!last_.valid)
      last_ = p;
  }
  auto p = last_;
  auto now = epochMs();
  if (p.timestamp && (p.timestamp > now + 50 ||
                      (now > p.timestamp && now - p.timestamp > timeout_))) {
    p.connected = p.valid = false;
    p.calibration = "UNKNOWN";
    p.status = "OpenTrack pose timeout";
  }
  return p;
}
} // namespace xr
