// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace xr {
// Notify from the main process so Type=notify can gate monitor power changes.
inline void notifyService(const std::string &message) {
  const auto *path = getenv("NOTIFY_SOCKET");
  if (!path || !*path)
    return;
  const std::string name(path);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (name.size() >= sizeof(address.sun_path))
    throw std::runtime_error("notification socket path too long");
  name.copy(address.sun_path, name.size());
  const bool abstract = name.front() == '@';
  if (abstract)
    address.sun_path[0] = '\0';
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("notification socket failed");
  const auto sent = sendto(fd, message.data(), message.size(), MSG_NOSIGNAL,
                           reinterpret_cast<sockaddr *>(&address),
                           offsetof(sockaddr_un, sun_path) + name.size() +
                               (abstract ? 0 : 1));
  close(fd);
  if (sent != static_cast<ssize_t>(message.size()))
    throw std::runtime_error("service readiness notification failed");
}
inline void notifyReady(bool preview) {
  notifyService(preview ? "READY=1\nSTATUS=XR preview ready"
                        : "READY=1\nSTATUS=XR display ready");
}
inline std::string socketPath() {
  auto r = getenv("XDG_RUNTIME_DIR");
  if (!r)
    throw std::runtime_error("XDG_RUNTIME_DIR unset");
  return std::string(r) + "/xr-hyprland.sock";
}
struct Control {
  int fd = -1, lock = -1;
  std::string path;
  Control() {
    path = socketPath();
    lock = open((path + ".lock").c_str(),
                O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
      throw std::runtime_error(
          "another xr-hyprland instance is running or lock unavailable");
    if (path.size() >= sizeof(sockaddr_un::sun_path))
      throw std::runtime_error("socket path too long");
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      throw std::runtime_error("control socket failed");
    unlink(path.c_str());
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    path.copy(a.sun_path, path.size());
    if (bind(fd, (sockaddr *)&a, sizeof a))
      throw std::runtime_error("control socket bind failed");
    chmod(path.c_str(), 0600);
  }
  ~Control() {
    if (fd >= 0) {
      close(fd);
      unlink(path.c_str());
    }
    if (lock >= 0)
      close(lock);
  }
  std::string receive() {
    char b[128];
    auto n = recv(fd, b, sizeof b, 0);
    return n > 0 ? std::string(b, n) : std::string{};
  }
};
} // namespace xr
