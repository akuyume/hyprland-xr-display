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
    bool preview = false, stereoPreview = false, flat = false, debug = false;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--config" && i + 1 < argc)
        config.load(argv[++i]);
      else if (a == "--preview")
        preview = true;
      else if (a == "--stereo-preview")
        preview = stereoPreview = true;
      else if (a == "--flat")
        flat = true;
      else if (a == "--debug")
        debug = true;
      else if (a == "--seconds" && i + 1 < argc)
        seconds = std::stod(argv[++i]);
      else {
        std::cout << "xr-hyprland [--config FILE] [--preview|--stereo-preview] "
                     "[--flat] "
                     "[--debug] [--seconds N]\n";
        return a == "--help" ? 0 : 2;
      }
    }
    if (stereoPreview)
      config.stereo_mode = "sbs";
    config.validate();
    signal(SIGINT, [](int) { running = 0; });
    signal(SIGTERM, [](int) { running = 0; });
    xr::Control ctl;
    std::unique_ptr<xr::PoseProvider> provider;
    if (!flat) {
      if (config.pose_backend == "breezy")
        provider = std::make_unique<xr::BreezyPose>(config.pose_path,
                                                    config.pose_timeout_ms);
      else
        provider = std::make_unique<xr::OpenTrackPose>(config.opentrack_port,
                                                       config.pose_timeout_ms);
    }
    xr::VirtualOutput output(config);
    xr::Display display(config, output.name, preview, flat);
    xr::HeadTracking tracking(config.pose_timeout_ms);
    bool notified = false;
    std::string desktopGeneration;
    std::string status;
    auto start = std::chrono::steady_clock::now(), report = start;
    while (running) {
      display.dispatch(1);
      display.maintain();
      auto pose = provider ? provider->sample() : xr::Pose{};
      switch (tracking.update(pose)) {
      case xr::HeadTracking::Change::acquired:
        std::cerr << "initial reference captured\n";
        break;
      case xr::HeadTracking::Change::resumed:
        std::cerr << "tracking resumed; retained frozen screen orientation\n";
        break;
      case xr::HeadTracking::Change::referenceReset:
        std::cerr << "tracker reference reset; retained screen orientation\n";
        break;
      case xr::HeadTracking::Change::none:
        break;
      }
      if (provider && pose.status != status &&
          pose.status != "inconsistent pose record") {
        status = pose.status;
        std::cerr << "pose: " << status << "\n";
      }
      auto command = ctl.receive();
      if (command == "stop")
        break;
      if (command == "recenter") {
        if (tracking.recenter(pose))
          std::cerr << "recentered\n";
        else
          std::cerr << "recenter refused: no current valid pose\n";
      }
      if (command.starts_with("ipd ")) {
        try {
          size_t end = 0;
          const auto value = command.substr(4);
          const double millimeters = std::stod(value, &end);
          if (end != value.size())
            throw std::runtime_error("invalid IPD value");
          display.setIpd(millimeters);
        } catch (const std::exception &error) {
          std::cerr << "IPD change refused: " << error.what() << '\n';
        }
      }
      // Camera-to-reference rotation maps camera rays onto the world-fixed
      // plane.
      display.render(tracking.rotation());
      if (!notified && display.ready() && (flat || pose.valid)) {
        xr::notifyReady(preview);
        notified = true;
      }
      if (notified) {
        const auto generation = display.desktopGeneration();
        if (!generation.empty() && generation != desktopGeneration) {
          xr::notifyService(std::string(preview
                                            ? "STATUS=XR preview desktop ready:"
                                            : "STATUS=XR desktop ready:") +
                            generation);
          desktopGeneration = generation;
        }
      }
      auto now = std::chrono::steady_clock::now();
      if (now - report >= std::chrono::seconds(debug ? 1 : 10)) {
        std::cerr << display.statistics();
        if (debug && provider) {
          auto a = xr::euler(tracking.orientation());
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
