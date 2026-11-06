#include "control.hpp"
#include <iostream>
int main(int argc, char **argv) {
  try {
    if (argc != 2 || (std::string(argv[1]) != "recenter" &&
                      std::string(argv[1]) != "stop")) {
      std::cerr << "usage: xr-hyprlandctl recenter|stop\n";
      return 2;
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
    auto n = sendto(s, argv[1], strlen(argv[1]), 0, (sockaddr *)&a, sizeof a);
    close(s);
    if (n < 0)
      throw std::runtime_error("renderer unavailable");
    std::cout << "command delivered\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
