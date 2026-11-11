// SPDX-License-Identifier: GPL-3.0-only
#include "hyprland.hpp"
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
namespace xr {
nlohmann::json hyprQuery(const std::string &name) {
  if (name != "clients" && name != "monitors")
    throw std::runtime_error("unsupported scene query");
  const auto *runtime = getenv("XDG_RUNTIME_DIR");
  const auto *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!runtime || !signature)
    throw std::runtime_error("Hyprland session environment missing");
  const auto path =
      std::string(runtime) + "/hypr/" + signature + "/.socket.sock";
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path))
    throw std::runtime_error("Hyprland socket path too long");
  path.copy(address.sun_path, path.size());
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    throw std::runtime_error("Hyprland scene socket failed");
  try {
    int result =
        connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (result && errno != EINPROGRESS && errno != EAGAIN)
      throw std::runtime_error("Hyprland scene connection failed");
    pollfd socketPoll{fd, POLLOUT, 0};
    if (poll(&socketPoll, 1, 200) <= 0)
      throw std::runtime_error("Hyprland scene connection timeout");
    const auto request = "j/" + name;
    if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
        ssize_t(request.size()))
      throw std::runtime_error("Hyprland scene request failed");
    std::string text;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
      socketPoll.events = POLLIN;
      if (poll(&socketPoll, 1, 20) < 0) {
        if (errno == EINTR)
          continue;
        throw std::runtime_error("Hyprland scene read failed");
      }
      char buffer[8192];
      const auto count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (count == 0) {
        close(fd);
        fd = -1;
        return nlohmann::json::parse(text);
      }
      if (count < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        throw std::runtime_error("Hyprland scene read failed");
      }
      text.append(buffer, count);
      if (text.size() > 8 * 1024 * 1024)
        throw std::runtime_error("Hyprland scene too large");
    }
    throw std::runtime_error("Hyprland scene response timeout");
  } catch (...) {
    if (fd >= 0)
      close(fd);
    throw;
  }
}
std::string hypr(const std::vector<std::string> &args) {
  int p[2];
  if (pipe(p))
    throw std::runtime_error("hyprctl pipe failed");
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    dup2(p[1], 1);
    dup2(p[1], 2);
    close(p[0]);
    close(p[1]);
    std::vector<char *> av{const_cast<char *>("hyprctl")};
    for (auto &a : args)
      av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    execvp(av[0], av.data());
    _exit(127);
  }
  close(p[1]);
  std::string out;
  char buf[4096];
  // Bounded local subprocess, including stalled compositor IPC.
  for (;;) {
    pollfd f{p[0], POLLIN, 0};
    int ready = poll(&f, 1, 2000);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      close(p[0]);
      throw std::runtime_error("hyprctl timed out");
    }
    ssize_t n = read(p[0], buf, sizeof buf);
    if (n <= 0)
      break;
    out.append(buf, n);
    if (out.size() > 1024 * 1024) {
      kill(pid, SIGKILL);
      break;
    }
  }
  close(p[0]);
  int s = 0;
  while (waitpid(pid, &s, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(s) || WEXITSTATUS(s) != 0)
    throw std::runtime_error("hyprctl: " + out);
  return out;
}
nlohmann::json monitors() {
  return nlohmann::json::parse(hypr({"-j", "monitors", "all"}));
}
static bool headless(const nlohmann::json &m) {
  auto make = m.value("make", std::string{}),
       model = m.value("model", std::string{});
  return (make.empty() && model.empty() &&
          m.value("description", std::string{}).empty() &&
          m.value("physicalWidth", 0) == 0 &&
          m.value("physicalHeight", 0) == 0) ||
         make == "Headless" || model == "Headless" ||
         m.value("description", std::string{}).find("Headless") !=
             std::string::npos;
}
VirtualOutput::VirtualOutput(const Config &c) {
  auto before = monitors();
  for (auto &m : before)
    if (m["name"] == c.virtual_output_name) {
      if (!headless(m))
        throw std::runtime_error("refusing to attach non-headless output " +
                                 c.virtual_output_name);
      if (m.value("disabled", false) ||
          std::to_string(m["width"].get<int>()) + "x" +
                  std::to_string(m["height"].get<int>()) !=
              c.virtual_resolution ||
          m["scale"].get<double>() != c.virtual_scale ||
          std::abs(m["refreshRate"].get<double>() - c.virtual_refresh_rate) > 1)
        throw std::runtime_error("existing virtual output has different mode; "
                                 "configure it explicitly before attaching");
      name = c.virtual_output_name;
      id_ = m["id"];
      std::cerr << "attached existing headless output " << name
                << " (will preserve)\n";
      return;
    }
  auto reply = hypr({"output", "create", "headless", c.virtual_output_name});
  if (reply.find("ok") == std::string::npos)
    throw std::runtime_error("headless creation failed: " + reply);
  auto after = monitors();
  std::vector<nlohmann::json> added;
  for (auto &m : after) {
    bool existed = false;
    for (auto &old : before)
      if (m["name"] == old["name"])
        existed = true;
    if (!existed && headless(m))
      added.push_back(m);
  }
  // Prefer explicitly requested name; otherwise accept only an unambiguous new
  // output.
  for (auto &m : added)
    if (m["name"] == c.virtual_output_name) {
      name = m["name"];
      id_ = m["id"];
    }
  if (name.empty() && added.size() == 1) {
    name = added[0]["name"];
    id_ = added[0]["id"];
  }
  if (name.empty())
    throw std::runtime_error("cannot safely identify created headless output");
  owned_ = true;
  try {
    std::string result;
    try {
      result = hypr({"keyword", "monitor",
                     name + "," + c.virtual_resolution + "@" +
                         std::to_string(c.virtual_refresh_rate) + ",auto,1"});
    } catch (const std::exception &e) {
      if (std::string(e.what()).find("non-legacy parsers") == std::string::npos)
        throw;
      result = "non-legacy parsers";
    }
    if (result.find("non-legacy parsers") != std::string::npos) {
      // The discovered connector must be safe to embed in a Lua string too.
      if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrs"
                                 "tuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("unsafe connector name");
      result = hypr({"eval", "hl.monitor({output=\"" + name + "\",mode=\"" +
                                 c.virtual_resolution + "@" +
                                 std::to_string(c.virtual_refresh_rate) +
                                 "\",position=\"auto\",scale=1})"});
    }
    if (result.find("ok") == std::string::npos)
      throw std::runtime_error(result);
  } catch (...) {
    hypr({"output", "remove", name});
    owned_ = false;
    throw;
  }
  std::cerr << "created headless output " << name << " " << c.virtual_resolution
            << " @ " << c.virtual_refresh_rate << " scale 1\n";
}
VirtualOutput::~VirtualOutput() {
  if (!owned_)
    return;
  try {
    for (auto &m : monitors())
      if (m["name"] == name && m["id"] == id_ && headless(m)) {
        hypr({"output", "remove", name});
        std::cerr << "removed owned output " << name << "\n";
        break;
      }
  } catch (const std::exception &e) {
    std::cerr << "cleanup: " << e.what() << "\n";
  }
}
} // namespace xr
