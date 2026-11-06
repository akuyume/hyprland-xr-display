// SPDX-License-Identifier: GPL-3.0-only
#include "config.hpp"
#include "control.hpp"
#include "hyprland.hpp"
#include "wayland.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
static volatile sig_atomic_t running = 1;
int main(int argc, char **argv) {
  try {
    xr::Config config;
    bool preview = false, flat = false, debug = false;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--config" && i + 1 < argc)
        config.load(argv[++i]);
      else if (a == "--preview")
        preview = true;
      else if (a == "--flat")
        flat = true;
      else if (a == "--debug")
        debug = true;
      else if (a == "--seconds" && i + 1 < argc)
        seconds = std::stod(argv[++i]);
      else {
        std::cout << "xr-hyprland [--config FILE] [--preview] [--flat] "
                     "[--debug] [--seconds N]\n";
        return a == "--help" ? 0 : 2;
      }
    }
    config.validate();
    signal(SIGINT, [](int) { running = 0; });
    signal(SIGTERM, [](int) { running = 0; });
    xr::Control ctl;
    xr::VirtualOutput output(config);
    xr::Display display(config, output.name, preview, flat);
    std::unique_ptr<xr::PoseProvider> provider;
    if (config.pose_backend == "breezy")
      provider = std::make_unique<xr::BreezyPose>(config.pose_path,
                                                  config.pose_timeout_ms);
    else
      provider = std::make_unique<xr::OpenTrackPose>(config.opentrack_port,
                                                     config.pose_timeout_ms);
    xr::Quat reference, last;
    bool haveReference = false, trackingLost = false;
    std::string status;
    auto start = std::chrono::steady_clock::now(), report = start;
    while (running) {
      display.dispatch(1);
      display.maintain();
      auto pose = provider->sample();
      if (pose.valid) {
        if (haveReference && trackingLost) {
          auto frozenDelta = reference.inverse() * last;
          reference = pose.orientation * frozenDelta.inverse();
          std::cerr << "tracking resumed; retained frozen screen orientation\n";
        }
        trackingLost = false;
        last = pose.orientation;
        if (!haveReference) {
          reference = last;
          haveReference = true;
          std::cerr << "initial reference captured\n";
        }
      }
      if (!pose.valid && pose.status != "inconsistent pose record")
        trackingLost = true;
      if (pose.status != status && pose.status != "inconsistent pose record") {
        status = pose.status;
        std::cerr << "pose: " << status << "\n";
      }
      auto command = ctl.receive();
      if (command == "stop")
        break;
      if (command == "recenter") {
        if (pose.valid) {
          reference = last;
          haveReference = true;
          std::cerr << "recentered\n";
        } else
          std::cerr << "recenter refused: no current valid pose\n";
      }
      // Camera-to-reference rotation maps camera rays onto the world-fixed
      // plane.
      display.render(haveReference ? (reference.inverse() * last).normalized()
                                   : xr::Quat{});
      auto now = std::chrono::steady_clock::now();
      if (now - report >= std::chrono::seconds(debug ? 1 : 10)) {
        std::cerr << display.statistics();
        if (debug) {
          auto a = xr::euler(last);
          std::cerr << " yaw/pitch/roll=" << a[0] << "," << a[1] << "," << a[2];
        }
        std::cerr << "\n";
        report = now;
      }
      if (seconds > 0 &&
          std::chrono::duration<double>(now - start).count() >= seconds)
        break;
    }
    std::cerr << "final: " << display.statistics() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "xr-hyprland: " << e.what() << '\n';
    return 1;
  }
}
