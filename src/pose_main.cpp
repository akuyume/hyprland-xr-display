#include "pose.hpp"
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
static volatile sig_atomic_t running = 1;
int main(int argc, char **argv) {
  std::string path = "/dev/shm/breezy_desktop_imu";
  double seconds = 0;
  std::string backend = "opentrack";
  int port = 4242;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--backend" && i + 1 < argc)
      backend = argv[++i];
    else if (a == "--port" && i + 1 < argc)
      port = std::stoi(argv[++i]);
    else if (a == "--path" && i + 1 < argc)
      path = argv[++i];
    else if (a == "--seconds" && i + 1 < argc)
      seconds = std::stod(argv[++i]);
    else {
      std::cout << "xr-pose [--backend opentrack|breezy] [--port 4242] [--path "
                   "FILE] [--seconds N]\n";
      return a == "--help" ? 0 : 2;
    }
  }
  signal(SIGINT, [](int) { running = 0; });
  signal(SIGTERM, [](int) { running = 0; });
  std::unique_ptr<xr::PoseProvider> provider;
  try {
    if (backend == "breezy")
      provider = std::make_unique<xr::BreezyPose>(path);
    else if (backend == "opentrack")
      provider = std::make_unique<xr::OpenTrackPose>(port);
    else
      throw std::runtime_error("unknown pose backend");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  auto start = std::chrono::steady_clock::now(), report = start;
  uint64_t last = 0;
  int updates = 0;
  bool any = false;
  while (running) {
    auto p = provider->sample();
    auto now = std::chrono::steady_clock::now();
    if (p.valid && p.timestamp != last) {
      last = p.timestamp;
      ++updates;
      any = true;
    }
    if (now - report >= std::chrono::milliseconds(500)) {
      auto q = p.orientation;
      auto a = xr::euler(q);
      double dt = std::chrono::duration<double>(now - report).count();
      std::cout << std::fixed << std::setprecision(4) << "q(xyzw)=" << q.x
                << "," << q.y << "," << q.z << "," << q.w
                << " yaw/pitch/roll=" << a[0] << "," << a[1] << "," << a[2]
                << " sampled_updates_hz=" << updates / dt
                << " connected=" << p.connected
                << " calibration=" << p.calibration
                << " timestamp=" << p.timestamp << " " << p.status << std::endl;
      updates = 0;
      report = now;
    }
    if (seconds > 0 &&
        std::chrono::duration<double>(now - start).count() >= seconds)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return any ? 0 : 1;
}
