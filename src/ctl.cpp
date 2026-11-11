#include "control.hpp"
#include "windows.hpp"
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "windows") {
      auto result = nlohmann::json::array();
      for (const auto &window : xr::listWindows())
        result.push_back({{"identifier", window.identifier},
                          {"address", window.address},
                          {"app_id", window.appId},
                          {"title", window.title}});
      std::cout << result.dump() << '\n';
      return 0;
    }
    const bool ipd = argc == 3 && std::string(argv[1]) == "ipd";
    if (!ipd && (argc != 2 || (std::string(argv[1]) != "recenter" &&
                               std::string(argv[1]) != "stop"))) {
      std::cerr
          << "usage: xr-hyprlandctl recenter|stop|windows|ipd MILLIMETERS\n";
      return 2;
    }
    std::string command = argv[1];
    if (ipd) {
      size_t end;
      double value = std::stod(argv[2], &end);
      if (end != strlen(argv[2]) || !std::isfinite(value) || value < 45 ||
          value > 80)
        throw std::runtime_error("IPD must be between 45 and 80 mm");
      command += " " + std::string(argv[2]);
    }
    auto p = xr::socketPath();
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (p.size() >= sizeof a.sun_path)
      throw std::runtime_error("socket path too long");
    p.copy(a.sun_path, p.size());
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
      throw std::runtime_error("socket failed");
    auto n =
        sendto(s, command.data(), command.size(), 0, (sockaddr *)&a, sizeof a);
    close(s);
    if (n < 0)
      throw std::runtime_error("renderer unavailable");
    std::cout << "command delivered\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
