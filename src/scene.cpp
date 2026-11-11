// SPDX-License-Identifier: GPL-3.0-only
#include "scene.hpp"
#include "hyprland.hpp"
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace xr {
struct Scene::Impl {
  std::string source;
  mutable std::mutex mutex;
  std::condition_variable_any wake;
  nlohmann::json clients = nlohmann::json::array(), monitor;
  nlohmann::json settings = nlohmann::json::object();
  std::string error;
  std::string generation;
  std::jthread worker;

  explicit Impl(std::string output)
      : source(std::move(output)), worker([this](std::stop_token stop) {
          while (!stop.stop_requested()) {
            try {
              nlohmann::json settingsCopy;
              {
                std::lock_guard lock(mutex);
                settingsCopy = settings;
              }
              const auto outputs = hyprQuery("monitors");
              const auto allClients = hyprQuery("clients");
              nlohmann::json selected = nlohmann::json::array(), current,
                             anchor;
              for (const auto &output : outputs)
                if (output.value("name", "") == source)
                  current = output;
              const auto desktop =
                  settingsCopy.value("desktop", nlohmann::json::object());
              const auto clones =
                  desktop.value("outputs", nlohmann::json::object());
              bool desktopReady =
                  desktop.value("phase", "") == "ready" && !clones.empty();
              if (!clones.empty()) {
                anchor = desktop.at("anchor");
                anchor["width"] = anchor.at("width").get<double>() /
                                  anchor.at("scale").get<double>();
                anchor["height"] = anchor.at("height").get<double>() /
                                   anchor.at("scale").get<double>();
                for (const auto &[name, saved] : clones.items()) {
                  auto found =
                      std::ranges::find_if(outputs, [&](const auto &output) {
                        return output.at("name") == name;
                      });
                  if (found == outputs.end() ||
                      found->at("id") != saved.at("id") ||
                      found->at("activeWorkspace").at("id") !=
                          saved.at("workspace").at("id"))
                    desktopReady = false;
                }
              }
              for (const auto &output : outputs) {
                const auto name = output.at("name").get<std::string>();
                const auto clone = clones.find(name);
                if (name != source && clone == clones.end())
                  continue;
                if (clone != clones.end() &&
                    (output.at("id") != clone->at("id") ||
                     output.value("physicalWidth", 0) != 0))
                  continue;
                const bool fullscreen =
                    std::ranges::any_of(allClients, [&](const auto &client) {
                      return client.value("mapped", false) &&
                             !client.value("hidden", false) &&
                             client.value("monitor", -1) == output.at("id") &&
                             client.at("workspace").at("id") ==
                                 output.at("activeWorkspace").at("id") &&
                             client.value("fullscreen", 0) > 0;
                    });
                for (const auto &client : allClients) {
                  if (!client.value("mapped", false) ||
                      client.value("hidden", false) ||
                      client.value("class", "") == "xr-hyprland-preview" ||
                      client.value("monitor", -1) != output.at("id") ||
                      client.at("workspace").at("id") !=
                          output.at("activeWorkspace").at("id"))
                    continue;
                  if (fullscreen && !client.value("floating", false) &&
                      client.value("fullscreen", 0) == 0)
                    continue;
                  auto projected = client;
                  if (clone != clones.end()) {
                    const auto &original = clone->at("original");
                    projected["at"] = {client.at("at").at(0).get<double>() -
                                           output.at("x").get<double>() +
                                           original.at("x").get<double>(),
                                       client.at("at").at(1).get<double>() -
                                           output.at("y").get<double>() +
                                           original.at("y").get<double>()};
                  } else if (!anchor.is_null()) {
                    projected["at"] = {
                        client.at("at").at(0).get<double>() -
                            output.at("x").get<double>() +
                            anchor.at("x").get<double>() +
                            (anchor.at("width").get<double>() -
                             output.at("width").get<double>() /
                                 output.at("scale").get<double>()) /
                                2,
                        client.at("at").at(1).get<double>() -
                            output.at("y").get<double>() +
                            anchor.at("y").get<double>() +
                            (anchor.at("height").get<double>() -
                             output.at("height").get<double>() /
                                 output.at("scale").get<double>()) /
                                2};
                  }
                  selected.push_back(std::move(projected));
                }
              }
              std::lock_guard lock(mutex);
              clients = std::move(selected);
              monitor =
                  anchor.is_null() ? std::move(current) : std::move(anchor);
              generation = desktopReady
                               ? desktop.at("generation").get<std::string>()
                               : "";
              error.clear();
            } catch (const std::exception &failure) {
              std::lock_guard lock(mutex);
              error = failure.what();
              // Compositor IPC can be delayed during layout changes or large
              // captures. Keep the last scene instead of flashing the desktop.
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(500),
                          [] { return false; });
          }
        }) {}
  ~Impl() {
    worker.request_stop();
    wake.notify_all();
  }
};

Scene::Scene(std::string source)
    : p(std::make_unique<Impl>(std::move(source))) {}
Scene::~Scene() = default;
std::vector<std::string> Scene::addresses() const {
  std::lock_guard lock(p->mutex);
  std::vector<std::string> result;
  for (const auto &client : p->clients)
    result.push_back(client.at("address"));
  return result;
}

void Scene::refreshSettings() {
  const auto *state = getenv("XDG_STATE_HOME");
  const auto *home = getenv("HOME");
  const auto root = state ? std::string(state)
                          : std::string(home ? home : "") + "/.local/state";
  std::ifstream file(root + "/xr-hyprland/windows.json");
  nlohmann::json settings = nlohmann::json::object();
  if (file) {
    settings = nlohmann::json::parse(file, nullptr, false);
    if (!settings.is_object())
      settings = nlohmann::json::object();
  }
  std::lock_guard lock(p->mutex);
  p->settings = std::move(settings);
}

std::string
Scene::readyGeneration(const std::vector<WindowImage> &images) const {
  std::lock_guard lock(p->mutex);
  for (const auto &client : p->clients)
    if (std::ranges::none_of(images, [&](const auto &image) {
          return image.ready &&
                 image.address == client.at("address").get<std::string>();
        }))
      return {};
  return p->generation;
}

std::vector<Panel> Scene::panels(const std::vector<WindowImage> &images,
                                 const Config &cfg) const {
  std::lock_guard lock(p->mutex);
  std::vector<Panel> result;
  if (p->monitor.is_null())
    return result;
  const double metersPerPixel = 2 * cfg.display_distance *
                                std::tan(rad(cfg.display_size / 2)) /
                                p->monitor.at("width").get<double>();
  for (const auto &client : p->clients) {
    const auto address = client.at("address").get<std::string>();
    const auto found = std::ranges::find_if(images, [&](const auto &image) {
      return image.ready && image.address == address;
    });
    if (found == images.end())
      continue;
    Panel panel;
    panel.image = *found;
    const auto width = client.at("size").at(0).get<double>();
    const auto height = client.at("size").at(1).get<double>();
    panel.halfWidth = width * metersPerPixel / 2;
    panel.halfHeight = height * metersPerPixel / 2;
    panel.center = {(client.at("at").at(0).get<double>() -
                     p->monitor.at("x").get<double>() + width / 2 -
                     p->monitor.at("width").get<double>() / 2) *
                        metersPerPixel,
                    -(client.at("at").at(1).get<double>() -
                      p->monitor.at("y").get<double>() + height / 2 -
                      p->monitor.at("height").get<double>() / 2) *
                        metersPerPixel,
                    -cfg.display_distance};
    const auto placements = p->settings.find("placements");
    if (placements != p->settings.end() && placements->is_object()) {
      const auto placement = placements->find(found->identifier);
      if (placement != placements->end() && placement->is_object()) {
        const auto number = [&](const char *key, double fallback) {
          const auto field = placement->find(key);
          return field != placement->end() && field->is_number()
                     ? field->get<double>()
                     : fallback;
        };
        const double yaw = number("yaw", 0.), pitch = number("pitch", 0.);
        const double distance = number("distance", cfg.display_distance);
        const double scale = number("scale", 1.);
        if (std::isfinite(yaw) && std::abs(yaw) <= 180 &&
            std::isfinite(pitch) && std::abs(pitch) <= 80 &&
            std::isfinite(distance) && distance >= .1 && distance <= 100 &&
            std::isfinite(scale) && scale >= .25 && scale <= 4) {
          panel.rotation = Quat::axis({0, 1, 0}, rad(yaw)) *
                           Quat::axis({1, 0, 0}, rad(pitch));
          panel.center = panel.rotation.rotate({0, 0, -distance});
          panel.halfWidth *= scale;
          panel.halfHeight *= scale;
        }
      }
    }
    result.push_back(std::move(panel));
  }
  return result;
}
std::string Scene::status() const {
  std::lock_guard lock(p->mutex);
  return p->error;
}
} // namespace xr
