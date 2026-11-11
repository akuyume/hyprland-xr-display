// SPDX-License-Identifier: GPL-3.0-only
#include "windows.hpp"
#include "ext-foreign-toplevel-list-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "hyprland-toplevel-mapping-v1-client.h"
#include "shm.hpp"
#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace xr {
struct Windows::Impl {
  wl_registry *registry = nullptr;
  wl_shm *shm = nullptr;
  ext_foreign_toplevel_list_v1 *list = nullptr;
  ext_foreign_toplevel_image_capture_source_manager_v1 *sources = nullptr;
  ext_image_copy_capture_manager_v1 *copies = nullptr;
  hyprland_toplevel_mapping_manager_v1 *mapping = nullptr;
  uint64_t frames = 0, failures = 0;
  size_t nextWindow = 0;
  struct Window {
    Impl &owner;
    WindowImage image;
    ext_foreign_toplevel_handle_v1 *handle = nullptr;
    hyprland_toplevel_window_mapping_handle_v1 *mapped = nullptr;
    ext_image_capture_source_v1 *source = nullptr;
    ext_image_copy_capture_session_v1 *session = nullptr;
    ext_image_copy_capture_frame_v1 *frame = nullptr;
    wl_buffer *buffer = nullptr;
    void *pixels = MAP_FAILED;
    size_t bytes = 0;
    uint32_t width = 0, height = 0, format = UINT32_MAX;
    uint32_t bufferWidth = 0, bufferHeight = 0, bufferFormat = UINT32_MAX;
    uint32_t textureFormat = UINT32_MAX;
    bool closed = false, selected = false, constraints = false,
         transformed = false;
    std::chrono::steady_clock::time_point lastRequest{};
    explicit Window(Impl &owner) : owner(owner) {}
    void beginConstraints() {
      if (constraints) {
        width = height = 0;
        format = UINT32_MAX;
        constraints = false;
      }
    }
    void finish() {
      if (frame)
        ext_image_copy_capture_frame_v1_destroy(frame);
      frame = nullptr;
    }
    void clearBuffer() {
      if (buffer)
        wl_buffer_destroy(buffer);
      buffer = nullptr;
      if (pixels != MAP_FAILED)
        munmap(pixels, bytes);
      pixels = MAP_FAILED;
      bufferWidth = bufferHeight = 0;
    }
    void stop() {
      finish();
      if (session)
        ext_image_copy_capture_session_v1_destroy(session);
      session = nullptr;
      if (source)
        ext_image_capture_source_v1_destroy(source);
      source = nullptr;
      clearBuffer();
      if (image.texture)
        glDeleteTextures(1, &image.texture);
      image.texture = 0;
      image.width = image.height = 0;
      textureFormat = UINT32_MAX;
      image.ready = constraints = false;
      width = height = 0;
      format = UINT32_MAX;
    }
    ~Window() {
      stop();
      if (mapped)
        hyprland_toplevel_window_mapping_handle_v1_destroy(mapped);
      if (handle)
        ext_foreign_toplevel_handle_v1_destroy(handle);
    }
    void failed() {
      finish();
      image.ready = false;
      ++owner.failures;
      // Constraint changes may race a copy. Reallocate on the next request.
      clearBuffer();
      lastRequest =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    }
    void ready() {
      if (transformed) {
        failed();
        return;
      }
      const auto info = shmFormatInfo(bufferFormat);
      if (!image.texture)
        glGenTextures(1, &image.texture);
      glBindTexture(GL_TEXTURE_2D, image.texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      const auto upload = info.pixelBytes == 3 ? GL_RGB : GL_RGBA;
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      if (image.width != int(bufferWidth) ||
          image.height != int(bufferHeight) || textureFormat != bufferFormat)
        glTexImage2D(GL_TEXTURE_2D, 0, upload, bufferWidth, bufferHeight, 0,
                     upload, GL_UNSIGNED_BYTE, pixels);
      else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bufferWidth, bufferHeight,
                        upload, GL_UNSIGNED_BYTE, pixels);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      image.width = bufferWidth;
      image.height = bufferHeight;
      textureFormat = bufferFormat;
      image.swapRB = info.swapRB;
      image.opaque = info.pixelBytes == 3 ||
                     bufferFormat == WL_SHM_FORMAT_XRGB8888 ||
                     bufferFormat == WL_SHM_FORMAT_XBGR8888;
      image.ready = glGetError() == GL_NO_ERROR;
      if (image.ready)
        ++owner.frames;
      else
        ++owner.failures;
      finish();
    }
    bool allocate() {
      if (buffer && bufferWidth == width && bufferHeight == height &&
          bufferFormat == format)
        return true;
      clearBuffer();
      const auto info = shmFormatInfo(format);
      if (!info.pixelBytes || !width || !height || width > 8192 ||
          height > 8192)
        return false;
      const uint32_t stride = width * info.pixelBytes;
      bytes = size_t(stride) * height;
      int fd = memfd_create("xr-window-capture", MFD_CLOEXEC);
      if (fd < 0)
        return false;
      if (ftruncate(fd, bytes)) {
        close(fd);
        return false;
      }
      pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (pixels == MAP_FAILED) {
        close(fd);
        return false;
      }
      auto *pool = wl_shm_create_pool(owner.shm, fd, bytes);
      buffer =
          wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
      wl_shm_pool_destroy(pool);
      close(fd);
      bufferWidth = width;
      bufferHeight = height;
      bufferFormat = format;
      return buffer;
    }
  };
  std::vector<std::unique_ptr<Window>> windows;
  static const ext_foreign_toplevel_handle_v1_listener handleListener;
  static const ext_foreign_toplevel_list_v1_listener listListener;
  static const hyprland_toplevel_window_mapping_handle_v1_listener
      mappingListener;
  static const ext_image_copy_capture_session_v1_listener sessionListener;
  static const ext_image_copy_capture_frame_v1_listener frameListener;
  static const wl_registry_listener registryListener;

  void map(Window &window) {
    if (!mapping || window.mapped || window.closed)
      return;
    window.mapped =
        hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel(
            mapping, window.handle);
    hyprland_toplevel_window_mapping_handle_v1_add_listener(
        window.mapped, &mappingListener, &window);
  }
  void update(const std::vector<std::string> &addresses, double rate) {
    std::erase_if(windows, [](const auto &window) { return window->closed; });
    if (windows.empty())
      return;
    nextWindow %= windows.size();
    const auto now = std::chrono::steady_clock::now();
    for (size_t step = 0; step < windows.size(); ++step) {
      const auto index = (nextWindow + step) % windows.size();
      auto &window = *windows[index];
      map(window);
      const bool selected =
          std::ranges::find(addresses, window.image.address) != addresses.end();
      if (!selected) {
        if (window.selected)
          window.stop();
        window.selected = false;
        continue;
      }
      window.selected = true;
      if (!window.session) {
        window.source =
            ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
                sources, window.handle);
        window.session = ext_image_copy_capture_manager_v1_create_session(
            copies, window.source,
            EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
        ext_image_copy_capture_session_v1_add_listener(
            window.session, &sessionListener, &window);
      }
      if (window.frame || !window.constraints ||
          now - window.lastRequest < std::chrono::duration<double>(1. / rate))
        continue;
      if (!window.allocate())
        continue;
      window.transformed = false;
      window.frame =
          ext_image_copy_capture_session_v1_create_frame(window.session);
      ext_image_copy_capture_frame_v1_add_listener(window.frame, &frameListener,
                                                   &window);
      ext_image_copy_capture_frame_v1_attach_buffer(window.frame,
                                                    window.buffer);
      ext_image_copy_capture_frame_v1_damage_buffer(
          window.frame, 0, 0, window.bufferWidth, window.bufferHeight);
      ext_image_copy_capture_frame_v1_capture(window.frame);
      window.lastRequest = now;
      // Stagger large SHM copies so many native-resolution apps do not all
      // deliver uploads in one burst. Rotate fairly across the whole scene.
      nextWindow = (index + 1) % windows.size();
      break;
    }
  }
  ~Impl() {
    windows.clear();
    if (list)
      ext_foreign_toplevel_list_v1_destroy(list);
    if (mapping)
      hyprland_toplevel_mapping_manager_v1_destroy(mapping);
    if (sources)
      ext_foreign_toplevel_image_capture_source_manager_v1_destroy(sources);
    if (copies)
      ext_image_copy_capture_manager_v1_destroy(copies);
    if (shm)
      wl_shm_destroy(shm);
    if (registry)
      wl_registry_destroy(registry);
  }
};

const ext_foreign_toplevel_handle_v1_listener Windows::Impl::handleListener = {
    [](void *data, ext_foreign_toplevel_handle_v1 *) {
      static_cast<Window *>(data)->closed = true;
    },
    [](void *, ext_foreign_toplevel_handle_v1 *) {},
    [](void *data, ext_foreign_toplevel_handle_v1 *, const char *title) {
      static_cast<Window *>(data)->image.title = title;
    },
    [](void *data, ext_foreign_toplevel_handle_v1 *, const char *app) {
      static_cast<Window *>(data)->image.appId = app;
    },
    [](void *data, ext_foreign_toplevel_handle_v1 *, const char *id) {
      static_cast<Window *>(data)->image.identifier = id;
    },
};
const ext_foreign_toplevel_list_v1_listener Windows::Impl::listListener = {
    [](void *data, ext_foreign_toplevel_list_v1 *,
       ext_foreign_toplevel_handle_v1 *handle) {
      auto &owner = *static_cast<Impl *>(data);
      auto window = std::make_unique<Window>(owner);
      window->handle = handle;
      ext_foreign_toplevel_handle_v1_add_listener(handle, &handleListener,
                                                  window.get());
      owner.map(*window);
      owner.windows.push_back(std::move(window));
    },
    [](void *, ext_foreign_toplevel_list_v1 *) {},
};
const hyprland_toplevel_window_mapping_handle_v1_listener
    Windows::Impl::mappingListener = {
        [](void *data, hyprland_toplevel_window_mapping_handle_v1 *,
           uint32_t high, uint32_t low) {
          std::ostringstream address;
          address << "0x" << std::hex << ((uint64_t(high) << 32) | low);
          static_cast<Window *>(data)->image.address = address.str();
        },
        [](void *, hyprland_toplevel_window_mapping_handle_v1 *) {},
};
const ext_image_copy_capture_session_v1_listener
    Windows::Impl::sessionListener = {
        [](void *data, ext_image_copy_capture_session_v1 *, uint32_t width,
           uint32_t height) {
          auto &window = *static_cast<Window *>(data);
          window.beginConstraints();
          window.width = width;
          window.height = height;
          window.constraints = false;
        },
        [](void *data, ext_image_copy_capture_session_v1 *, uint32_t format) {
          auto &window = *static_cast<Window *>(data);
          window.beginConstraints();
          const auto rank = [](uint32_t candidate) {
            if (candidate == WL_SHM_FORMAT_ARGB8888 ||
                candidate == WL_SHM_FORMAT_ABGR8888)
              return 3;
            return shmFormatInfo(candidate).pixelBytes == 4   ? 2
                   : shmFormatInfo(candidate).pixelBytes == 3 ? 1
                                                              : 0;
          };
          if (rank(format) > rank(window.format))
            window.format = format;
        },
        [](void *, ext_image_copy_capture_session_v1 *, wl_array *) {},
        [](void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {
        },
        [](void *data, ext_image_copy_capture_session_v1 *) {
          static_cast<Window *>(data)->constraints = true;
        },
        [](void *data, ext_image_copy_capture_session_v1 *) {
          auto &window = *static_cast<Window *>(data);
          window.stop();
          window.closed = true;
        },
};
const ext_image_copy_capture_frame_v1_listener Windows::Impl::frameListener = {
    [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t transform) {
      static_cast<Window *>(data)->transformed = transform != 0;
    },
    [](void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t,
       int32_t) {},
    [](void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t,
       uint32_t) {},
    [](void *data, ext_image_copy_capture_frame_v1 *) {
      static_cast<Window *>(data)->ready();
    },
    [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t) {
      static_cast<Window *>(data)->failed();
    },
};
const wl_registry_listener Windows::Impl::registryListener = {
    [](void *data, wl_registry *registry, uint32_t name, const char *interface,
       uint32_t version) {
      auto &owner = *static_cast<Impl *>(data);
#define BIND(protocol, field)                                                  \
  if (std::strcmp(interface, #protocol) == 0) {                                \
    owner.field = static_cast<protocol *>(wl_registry_bind(                    \
        registry, name, &protocol##_interface, std::min(version, 1u)));        \
  }
      BIND(wl_shm, shm)
      BIND(ext_foreign_toplevel_image_capture_source_manager_v1, sources)
      BIND(ext_image_copy_capture_manager_v1, copies)
      BIND(hyprland_toplevel_mapping_manager_v1, mapping)
      BIND(ext_foreign_toplevel_list_v1, list)
#undef BIND
      if (std::strcmp(interface, "ext_foreign_toplevel_list_v1") == 0)
        ext_foreign_toplevel_list_v1_add_listener(owner.list, &listListener,
                                                  &owner);
    },
    [](void *, wl_registry *, uint32_t) {},
};

Windows::Windows(wl_display *display) : p(std::make_unique<Impl>()) {
  p->registry = wl_display_get_registry(display);
  wl_registry_add_listener(p->registry, &Impl::registryListener, p.get());
  for (int pass = 0; pass < 3; ++pass)
    if (wl_display_roundtrip(display) < 0)
      throw std::runtime_error("window discovery failed");
  for (auto &window : p->windows)
    p->map(*window);
  if (wl_display_roundtrip(display) < 0)
    throw std::runtime_error("window mapping failed");
}
Windows::~Windows() = default;
bool Windows::supported() const {
  return p->list && p->mapping && p->sources && p->copies && p->shm;
}
std::vector<WindowImage> Windows::images() const {
  std::vector<WindowImage> result;
  for (const auto &window : p->windows)
    if (!window->closed)
      result.push_back(window->image);
  return result;
}
void Windows::update(const std::vector<std::string> &addresses, double rate) {
  p->update(addresses, rate);
}
uint64_t Windows::frames() const { return p->frames; }
uint64_t Windows::failures() const { return p->failures; }
std::vector<WindowImage> listWindows() {
  auto *display = wl_display_connect(nullptr);
  if (!display)
    throw std::runtime_error("cannot connect to Wayland");
  std::vector<WindowImage> result;
  try {
    Windows windows(display);
    if (!windows.supported())
      throw std::runtime_error("individual-window protocols unavailable");
    result = windows.images();
  } catch (...) {
    wl_display_disconnect(display);
    throw;
  }
  wl_display_disconnect(display);
  return result;
}
} // namespace xr
