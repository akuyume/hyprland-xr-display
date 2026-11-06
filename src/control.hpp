// SPDX-License-Identifier: GPL-3.0-only
#pragma once
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
