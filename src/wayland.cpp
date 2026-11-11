// SPDX-License-Identifier: GPL-3.0-only
#include "wayland.hpp"
#include "linux-dmabuf-v1-client.h"
#include "scene.hpp"
#include "shaders.hpp"
#include "shm.hpp"
#include "stereo.hpp"
#include "wlr-layer-shell-unstable-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"
#include "xdg-shell-client.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// clang-format off
#include <GLES2/gl2ext.h>
// clang-format on
#include <algorithm>
#include <chrono>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-egl.h>
namespace xr {
static void require(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
struct Display::Impl {
  Config cfg;
  std::string source;
  bool preview, flat;
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_compositor *compositor = nullptr;
  wl_shm *shm = nullptr;
  zwlr_layer_shell_v1 *shell = nullptr;
  zwlr_screencopy_manager_v1 *copy = nullptr;
  zwp_linux_dmabuf_v1 *dma = nullptr;
  xdg_wm_base *xdg = nullptr;
  struct Output {
    Impl *owner;
    uint32_t id;
    wl_output *wl;
    std::string name, make, model;
    int width = 0, height = 0, scale = 1, transform = 0;
    bool alive = true;
  };
  std::map<uint32_t, std::unique_ptr<Output>> outputs;
  wl_surface *surface = nullptr;
  zwlr_layer_surface_v1 *layer = nullptr;
  xdg_surface *xs = nullptr;
  xdg_toplevel *top = nullptr;
  wl_egl_window *window = nullptr;
  wl_callback *callback = nullptr;
  Output *target = nullptr;
  Output *captured = nullptr;
  bool configured = false, closed = false, framePermit = true;
  bool contentPresented = false;
  int width = 1920, height = 1080;
  EGLDisplay egl = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface eglSurface = EGL_NO_SURFACE;
  EGLConfig eglConfig{};
  GLuint program = 0, vao = 0;
  int drmFd = -1;
  gbm_device *gbm = nullptr;
  PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTexture = nullptr;
  std::map<uint32_t, std::vector<uint64_t>> modifiers;
  uint64_t rendered = 0, capturedFrames = 0, failed = 0, dropped = 0;
  std::string error;
  std::chrono::steady_clock::time_point lastRender{}, lastRequest{};
  struct Slot {
    wl_buffer *buffer = nullptr;
    zwp_linux_buffer_params_v1 *params = nullptr;
    gbm_bo *bo = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;
    GLsync fence = nullptr;
    void *map = MAP_FAILED;
    size_t bytes = 0;
    uint32_t width = 0, height = 0, stride = 0, format = 0, pixelBytes = 0;
    bool gpu = false, swapRB = false, flip = false, waiting = false;
  } slots[2];
  std::vector<uint8_t> packedShmRows;
  int front = -1, pending = -1;
  zwlr_screencopy_frame_v1 *frame = nullptr;
  uint32_t sw = 0, sh = 0, stride = 0, shmFormat = 0, dw = 0, dh = 0,
           dmaFormat = 0;
  bool dmaOff = false, loggedPath = false;
  std::optional<bool> hardwareSbs;
  StereoLayout lastLayout = StereoLayout::waiting;
  bool layoutLogged = false;
  std::chrono::steady_clock::time_point lastStereoRead{};
  std::chrono::steady_clock::time_point lastSceneRead{};
  std::unique_ptr<Windows> windows;
  std::unique_ptr<Scene> scene;
  struct Capture final : CaptureBackend {
    Impl &a;
    explicit Capture(Impl &a) : a(a) {}
    void request() override { a.requestCapture(); }
    unsigned texture() const override {
      return a.front < 0 ? 0 : a.slots[a.front].tex;
    }
  } backend{*this};
  static const wl_output_listener outputListener;
  static const wl_registry_listener registryListener;
  static const zwp_linux_dmabuf_v1_listener dmaListener;
  static const xdg_wm_base_listener xdgListener;
  Impl(const Config &c, std::string src, bool prev, bool f)
      : cfg(c), source(std::move(src)), preview(prev), flat(f) {}
  void init() {
    display = wl_display_connect(nullptr);
    require(display, "cannot connect to Wayland");
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, this);
    require(wl_display_roundtrip(display) >= 0, "Wayland registry failed");
    require(compositor && shm && copy,
            "required Wayland compositor/shm/screencopy unavailable");
    require(wl_proxy_get_version((wl_proxy *)copy) >= 3,
            "screencopy v3 required");
    if (dma)
      zwp_linux_dmabuf_v1_add_listener(dma, &dmaListener, this);
    if (xdg)
      xdg_wm_base_add_listener(xdg, &xdgListener, this);
    require(wl_display_roundtrip(display) >= 0,
            "Wayland output discovery failed");
    egl = eglGetDisplay((EGLNativeDisplayType)display);
    require(egl != EGL_NO_DISPLAY && eglInitialize(egl, nullptr, nullptr),
            "EGL initialization failed");
    require(eglBindAPI(EGL_OPENGL_ES_API), "EGL ES API unavailable");
    EGLint attrs[] = {EGL_SURFACE_TYPE,
                      EGL_WINDOW_BIT,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES3_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_DEPTH_SIZE,
                      24,
                      EGL_NONE};
    EGLint count = 0;
    require(eglChooseConfig(egl, attrs, &eglConfig, 1, &count) && count,
            "EGL config unavailable");
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(egl, eglConfig, EGL_NO_CONTEXT, ca);
    require(context != EGL_NO_CONTEXT, "EGL context failed");
    require(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
            "surfaceless EGL unavailable");
    std::cerr << "render device: " << glGetString(GL_RENDERER) << "; GLES "
              << glGetString(GL_VERSION) << "; rate cap "
              << cfg.render_refresh_rate << " Hz\n";
    createImage =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroyImage =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    imageTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    drmFd = open(cfg.drm_device.c_str(), O_RDWR | O_CLOEXEC);
    if (drmFd >= 0)
      gbm = gbm_create_device(drmFd);
    dmaOff = cfg.capture_backend == "shm" || !gbm || !dma || !createImage ||
             !imageTexture;
    if (dmaOff && cfg.capture_backend == "dmabuf")
      throw std::runtime_error("requested DMA-BUF path unavailable");
    initShader();
    if (cfg.view_mode == "windows") {
      windows = std::make_unique<Windows>(display);
      require(windows->supported(),
              "individual-window capture protocols unavailable");
      scene = std::make_unique<Scene>(source);
    }
    maintain();
  }
  void initShader() {
    auto shader = [](GLenum type, const char *text) {
      GLuint s = glCreateShader(type);
      glShaderSource(s, 1, &text, nullptr);
      glCompileShader(s);
      GLint ok;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
        char b[2048];
        glGetShaderInfoLog(s, sizeof b, nullptr, b);
        glDeleteShader(s);
        throw std::runtime_error(b);
      }
      return s;
    };
    auto v = shader(GL_VERTEX_SHADER, vertexShader),
         f = shader(GL_FRAGMENT_SHADER, fragmentShader);
    program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    require(ok, "shader link failed");
    glGenVertexArrays(1, &vao);
  }
  static const zwlr_layer_surface_v1_listener layerListener;
  static const xdg_surface_listener xsListener;
  static const xdg_toplevel_listener topListener;
  void createSurface() {
    surface = wl_compositor_create_surface(compositor);
    auto region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);
    if (preview) {
      require(xdg, "xdg-shell unavailable");
      width = cfg.stereo_mode == "sbs" ? 1280 : 960;
      height = cfg.stereo_mode == "sbs" ? 360 : 540;
      xs = xdg_wm_base_get_xdg_surface(xdg, surface);
      xdg_surface_add_listener(xs, &xsListener, this);
      top = xdg_surface_get_toplevel(xs);
      xdg_toplevel_add_listener(top, &topListener, this);
      xdg_toplevel_set_title(top, "XR headless capture diagnostic");
      xdg_toplevel_set_app_id(top, "xr-hyprland-preview");
    } else {
      require(shell, "layer-shell unavailable");
      layer = zwlr_layer_shell_v1_get_layer_surface(
          shell, surface, target->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
          "xr-hyprland");
      zwlr_layer_surface_v1_add_listener(layer, &layerListener, this);
      zwlr_layer_surface_v1_set_anchor(layer, 15);
      zwlr_layer_surface_v1_set_size(layer, 0, 0);
      zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
      zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 0);
    }
    wl_surface_commit(surface);
    std::cerr << "surface: " << (preview ? "diagnostic window" : target->name)
              << "\n";
  }
  void destroySurface() {
    if (callback) {
      wl_callback_destroy(callback);
      callback = nullptr;
    }
    if (egl != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT)
      eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    if (eglSurface != EGL_NO_SURFACE) {
      eglDestroySurface(egl, eglSurface);
      eglSurface = EGL_NO_SURFACE;
    }
    if (window) {
      wl_egl_window_destroy(window);
      window = nullptr;
    }
    if (layer) {
      zwlr_layer_surface_v1_destroy(layer);
      layer = nullptr;
    }
    if (top) {
      xdg_toplevel_destroy(top);
      top = nullptr;
    }
    if (xs) {
      xdg_surface_destroy(xs);
      xs = nullptr;
    }
    if (surface) {
      wl_surface_destroy(surface);
      surface = nullptr;
    }
    configured = false;
    framePermit = true;
    target = nullptr;
  }
  void maintain() {
    auto now = std::chrono::steady_clock::now();
    if (!preview && now - lastStereoRead >= std::chrono::milliseconds(250)) {
      hardwareSbs = readStereoState(cfg.driver_state_path, epochMs());
      lastStereoRead = now;
    }
    if (scene && now - lastSceneRead >= std::chrono::milliseconds(200)) {
      scene->refreshSettings();
      lastSceneRead = now;
    }
    if (closed || (target && !target->alive)) {
      destroySurface();
      closed = false;
      if (preview)
        throw std::runtime_error("preview closed");
    }
    if (captured && !captured->alive) {
      resetCapture();
      captured = nullptr;
    }
    if (!captured)
      for (auto &[id, o] : outputs)
        if (o->alive && o->name == source) {
          captured = o.get();
          std::cerr << "capture source: " << source << " (cursor included)\n";
        }
    if (!surface) {
      if (preview)
        createSurface();
      else {
        std::vector<Output *> matches;
        for (auto &[id, o] : outputs)
          if (o->alive && ((!cfg.target_output_name_override.empty() &&
                            o->name == cfg.target_output_name_override) ||
                           (cfg.target_output_name_override.empty() &&
                            o->make == cfg.target_output_make &&
                            o->model == cfg.target_output_model)))
            matches.push_back(o.get());
        if (matches.size() > 1)
          throw std::runtime_error(
              "multiple target displays; set target_output_name_override");
        if (matches.size() == 1) {
          target = matches[0];
          require(target->name != source, "capture feedback loop refused");
          createSurface();
        }
      }
    }
    if (configured && !window) {
      window = wl_egl_window_create(surface, width, height);
      require(window, "Wayland EGL window failed");
      eglSurface = eglCreateWindowSurface(egl, eglConfig,
                                          (EGLNativeWindowType)window, nullptr);
      require(eglSurface != EGL_NO_SURFACE, "EGL window surface failed");
      require(eglMakeCurrent(egl, eglSurface, eglSurface, context),
              "EGL make current failed");
      eglSwapInterval(egl, 0);
    }
    // Removed globals cannot accumulate indefinitely across repeated hotplug.
    for (auto i = outputs.begin(); i != outputs.end();)
      if (!i->second->alive && i->second.get() != target &&
          i->second.get() != captured) {
        wl_output_release(i->second->wl);
        i = outputs.erase(i);
      } else
        ++i;
    if (!error.empty())
      throw std::runtime_error(error);
    if (window && windows)
      windows->update(scene->addresses(), cfg.virtual_refresh_rate);
    if (window && captured && (!scene || scene->addresses().empty()))
      backend.request();
  }
  void clearSlot(Slot &s) {
    if (s.fence) {
      glDeleteSync(s.fence);
      s.fence = nullptr;
    }
    if (s.params)
      zwp_linux_buffer_params_v1_destroy(s.params);
    if (s.buffer)
      wl_buffer_destroy(s.buffer);
    if (s.tex)
      glDeleteTextures(1, &s.tex);
    if (s.image != EGL_NO_IMAGE_KHR && destroyImage)
      destroyImage(egl, s.image);
    if (s.bo)
      gbm_bo_destroy(s.bo);
    if (s.map != MAP_FAILED)
      munmap(s.map, s.bytes);
    s = Slot{};
  }
  void resetCapture() {
    if (frame) {
      zwlr_screencopy_frame_v1_destroy(frame);
      frame = nullptr;
    }
    glFinish();
    for (auto &s : slots)
      clearSlot(s);
    front = pending = -1;
  }
  static const zwlr_screencopy_frame_v1_listener frameListener;
  void requestCapture() {
    auto now = std::chrono::steady_clock::now();
    if (frame || now - lastRequest < std::chrono::duration<double>(
                                         1. / cfg.virtual_refresh_rate))
      return;
    pending = front == 0 ? 1 : 0;
    auto &s = slots[pending];
    if (s.fence) {
      GLenum state = glClientWaitSync(s.fence, 0, 0);
      if (state == GL_TIMEOUT_EXPIRED) {
        pending = -1;
        ++dropped;
        return;
      }
      require(state != GL_WAIT_FAILED, "GPU fence failed");
      glDeleteSync(s.fence);
      s.fence = nullptr;
    }
    dw = dh = sw = sh = 0;
    auto period =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1. / cfg.virtual_refresh_rate));
    lastRequest = now - lastRequest > period * 2 ? now : lastRequest + period;
    frame = zwlr_screencopy_manager_v1_capture_output(copy, 1, captured->wl);
    zwlr_screencopy_frame_v1_add_listener(frame, &frameListener, this);
  }
  void captureFailed() {
    ++failed;
    if (frame)
      zwlr_screencopy_frame_v1_destroy(frame);
    frame = nullptr;
    if (pending >= 0 && slots[pending].gpu && cfg.capture_backend == "auto") {
      dmaOff = true;
      std::cerr << "DMA-BUF capture failed; falling back to SHM upload\n";
      clearSlot(slots[pending]);
    } else if (cfg.capture_backend == "dmabuf")
      error = "DMA-BUF capture rejected";
    pending = -1;
  }
  static const zwp_linux_buffer_params_v1_listener paramsListener;
  void setupTexture(Slot &s) {
    glGenTextures(1, &s.tex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  bool gpuBuffer(Slot &s) {
    if (!dw || !dh || dw > 8192 || dh > 8192)
      return false;
    auto mods = modifiers[dmaFormat];
    // Try compositor-advertised explicit modifiers; GBM selects one supported
    // by the device.
    mods.erase(std::remove(mods.begin(), mods.end(), DRM_FORMAT_MOD_INVALID),
               mods.end());
    if (!mods.empty())
      s.bo = gbm_bo_create_with_modifiers2(gbm, dw, dh, dmaFormat, mods.data(),
                                           mods.size(), GBM_BO_USE_RENDERING);
    if (!s.bo)
      return false;
    if (gbm_bo_get_plane_count(s.bo) != 1)
      return false;
    int fd = gbm_bo_get_fd(s.bo);
    if (fd < 0)
      return false;
    auto modifier = gbm_bo_get_modifier(s.bo);
    auto pitch = gbm_bo_get_stride(s.bo);
    auto offset = gbm_bo_get_offset(s.bo, 0);
    EGLint attrs[] = {EGL_WIDTH,
                      int(dw),
                      EGL_HEIGHT,
                      int(dh),
                      EGL_LINUX_DRM_FOURCC_EXT,
                      int(dmaFormat),
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      int(offset),
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      int(pitch),
                      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                      int(modifier & 0xffffffff),
                      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                      int(modifier >> 32),
                      EGL_NONE};
    s.image =
        createImage(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (s.image == EGL_NO_IMAGE_KHR) {
      close(fd);
      return false;
    }
    setupTexture(s);
    while (glGetError() != GL_NO_ERROR) {
    }
    imageTexture(GL_TEXTURE_2D, s.image);
    if (glGetError() != GL_NO_ERROR) {
      close(fd);
      return false;
    }
    s.gpu = true;
    s.width = dw;
    s.height = dh;
    s.format = dmaFormat;
    s.stride = pitch;
    s.params = zwp_linux_dmabuf_v1_create_params(dma);
    zwp_linux_buffer_params_v1_add_listener(s.params, &paramsListener, this);
    zwp_linux_buffer_params_v1_add(s.params, fd, 0, offset, pitch,
                                   modifier >> 32, modifier & 0xffffffff);
    close(fd);
    s.waiting = true;
    zwp_linux_buffer_params_v1_create(s.params, dw, dh, dmaFormat, 0);
    return true;
  }
  void prepareBuffer() {
    auto &s = slots[pending];
    bool gpu = !dmaOff && dw && dh;
    if (s.buffer && s.gpu == gpu && s.width == (gpu ? dw : sw) &&
        s.height == (gpu ? dh : sh) &&
        s.format == (gpu ? dmaFormat : shmFormat) &&
        (gpu || s.stride == stride)) {
      s.flip = false;
      zwlr_screencopy_frame_v1_copy(frame, s.buffer);
      return;
    }
    clearSlot(s);
    if (gpu) {
      if (gpuBuffer(s))
        return;
      clearSlot(s);
      if (cfg.capture_backend == "dmabuf")
        throw std::runtime_error("GPU DMA-BUF allocation/import unavailable");
      dmaOff = true;
      std::cerr << "DMA-BUF allocation/import unavailable; using SHM upload\n";
    }
    const auto layout = shmFormatInfo(shmFormat);
    require(layout.pixelBytes != 0, "unsupported SHM format");
    require(sw > 0 && sh > 0 && sw <= 8192 && sh <= 8192,
            "invalid SHM capture dimensions");
    const uint32_t rowBytes = sw * layout.pixelBytes;
    require(stride >= rowBytes && stride <= rowBytes + 65536,
            "invalid SHM capture stride");
    const size_t packedBytes = size_t(rowBytes) * sh;
    if (stride % layout.pixelBytes != 0 && packedShmRows.size() < packedBytes)
      packedShmRows.resize(packedBytes);
    s.width = sw;
    s.height = sh;
    s.stride = stride;
    s.format = shmFormat;
    s.pixelBytes = layout.pixelBytes;
    s.swapRB = layout.swapRB;
    s.bytes = size_t(stride) * sh;
    int fd = memfd_create("xr-capture", MFD_CLOEXEC);
    require(fd >= 0, "capture memfd failed");
    if (ftruncate(fd, s.bytes)) {
      close(fd);
      throw std::runtime_error("capture allocation failed");
    }
    s.map = mmap(nullptr, s.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s.map == MAP_FAILED) {
      close(fd);
      throw std::runtime_error("capture mmap failed");
    }
    auto pool = wl_shm_create_pool(shm, fd, s.bytes);
    s.buffer = wl_shm_pool_create_buffer(pool, 0, sw, sh, stride, shmFormat);
    wl_shm_pool_destroy(pool);
    close(fd);
    setupTexture(s);
    const GLenum uploadFormat = s.pixelBytes == 3 ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, uploadFormat, sw, sh, 0, uploadFormat,
                 GL_UNSIGNED_BYTE, nullptr);
    zwlr_screencopy_frame_v1_copy(frame, s.buffer);
  }
  void ready() {
    auto &s = slots[pending];
    if (!s.gpu) {
      const GLenum uploadFormat = s.pixelBytes == 3 ? GL_RGB : GL_RGBA;
      const void *pixels = s.map;
      uint32_t rowLength = s.stride / s.pixelBytes;
      if (s.stride % s.pixelBytes != 0) {
        packShmRows(packedShmRows.data(), s.map, s.width * s.pixelBytes,
                    s.height, s.stride);
        pixels = packedShmRows.data();
        rowLength = s.width;
      }
      glBindTexture(GL_TEXTURE_2D, s.tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.width, s.height, uploadFormat,
                      GL_UNSIGNED_BYTE, pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    if (!loggedPath) {
      std::cerr << "capture backend: wlr-screencopy "
                << (s.gpu ? "DMA-BUF -> EGLImage (GPU resident)"
                          : "SHM -> GPU upload (diagnostic fallback)")
                << " format=0x" << std::hex << s.format << std::dec << " "
                << s.width << "x" << s.height << "\n";
      loggedPath = true;
    }
    front = pending;
    pending = -1;
    ++capturedFrames;
    zwlr_screencopy_frame_v1_destroy(frame);
    frame = nullptr;
  }
  static const wl_callback_listener callbackListener;
  void render(Quat delta) {
    auto now = std::chrono::steady_clock::now();
    if (!window || !framePermit ||
        now - lastRender <
            std::chrono::duration<double>(1. / cfg.render_refresh_rate))
      return;
    require(eglMakeCurrent(egl, eglSurface, eglSurface, context),
            "EGL render context lost");
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const auto layout = stereoLayout(
        cfg.stereo_mode, preview, width, height, target ? target->width : width,
        target ? target->height : height, target ? target->scale : 1,
        target ? target->transform : 0, hardwareSbs);
    if (!layoutLogged || layout != lastLayout) {
      std::cerr << "presentation: "
                << (layout == StereoLayout::sbs ? "stereo SBS"
                    : layout == StereoLayout::mono
                        ? "mono"
                        : "waiting for matching Air mode/transport")
                << "; IPD=" << cfg.ipd_mm << " mm\n";
      lastLayout = layout;
      layoutLogged = true;
    }
    bool drewContent = false;
    if (layout != StereoLayout::waiting) {
      glUseProgram(program);
      glBindVertexArray(vao);
      glActiveTexture(GL_TEXTURE0);
      auto uniform = [&](const char *n) {
        return glGetUniformLocation(program, n);
      };
      auto offset = Quat::axis({0, 1, 0}, rad(cfg.yaw_offset)) *
                    Quat::axis({1, 0, 0}, rad(cfg.pitch_offset));
      auto q =
          offset.inverse() * axisPolicy(delta, cfg.invert_yaw, cfg.invert_pitch,
                                        cfg.invert_roll, cfg.roll_enabled);
      glUniform4f(uniform("rotation"), q.x, q.y, q.z, q.w);
      glUniform2f(uniform("fov"), std::tan(rad(cfg.horizontal_fov / 2)),
                  std::tan(rad(cfg.vertical_fov / 2)));
      glUniform1i(uniform("flatView"), flat && !scene);
      glUniform1i(uniform("desktop"), 0);
      auto panels =
          scene ? scene->panels(windows->images(), cfg) : std::vector<Panel>{};
      std::ranges::sort(panels, [](const auto &a, const auto &b) {
        return a.center.x * a.center.x + a.center.y * a.center.y +
                   a.center.z * a.center.z >
               b.center.x * b.center.x + b.center.y * b.center.y +
                   b.center.z * b.center.z;
      });
      auto draw = [&](GLuint texture, Vec center, Quat rotation, double hw,
                      double hh, bool flip, bool swap, bool opaque) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform3f(uniform("planeCenter"), center.x, center.y, center.z);
        glUniform4f(uniform("planeRotation"), rotation.x, rotation.y,
                    rotation.z, rotation.w);
        glUniform2f(uniform("halfSize"), hw, hh);
        glUniform1i(uniform("flipY"), flip);
        glUniform1i(uniform("swapRB"), swap);
        glUniform1i(uniform("opaque"), opaque);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        drewContent = true;
      };
      const int eyes = layout == StereoLayout::sbs ? 2 : 1;
      const int eyeWidth = width / eyes;
      glEnable(GL_SCISSOR_TEST);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      for (int eye = 0; eye < eyes; ++eye) {
        glViewport(eye * eyeWidth, 0, eyeWidth, height);
        glScissor(eye * eyeWidth, 0, eyeWidth, height);
        glUniform1f(uniform("eyeOffset"),
                    eyes == 2 ? eyeOffset(eye, cfg.ipd_mm) : 0);
        if (scene && !scene->addresses().empty()) {
          for (const auto &panel : panels)
            draw(panel.image.texture, panel.center, panel.rotation,
                 panel.halfWidth, panel.halfHeight, false, panel.image.swapRB,
                 panel.image.opaque);
        } else if (front >= 0) {
          const auto &s = slots[front];
          const double hw =
              cfg.display_distance * std::tan(rad(cfg.display_size / 2));
          draw(backend.texture(), {0, 0, -cfg.display_distance}, {}, hw,
               hw * s.height / s.width, s.flip, s.swapRB, true);
        }
      }
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_BLEND);
      if (front >= 0) {
        auto &s = slots[front];
        if (s.fence)
          glDeleteSync(s.fence);
        s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      }
      glFlush();
    }
    require(glGetError() == GL_NO_ERROR, "OpenGL render/upload failed");
    callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &callbackListener, this);
    framePermit = false;
    require(eglSwapBuffers(egl, eglSurface), "EGL swap failed");
    contentPresented = drewContent;
    auto period =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1. / cfg.render_refresh_rate));
    lastRender = now - lastRender > period * 2 ? now : lastRender + period;
    ++rendered;
  }
  void dispatch(int timeout) {
    while (wl_display_prepare_read(display) != 0)
      require(wl_display_dispatch_pending(display) >= 0,
              "Wayland dispatch failed");
    int flush = wl_display_flush(display);
    if (flush < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      throw std::runtime_error("Wayland flush failed");
    }
    pollfd p{wl_display_get_fd(display),
             short(POLLIN | (flush < 0 ? POLLOUT : 0)), 0};
    int n = poll(&p, 1, timeout);
    if (n > 0 && (p.revents & POLLIN)) {
      require(wl_display_read_events(display) >= 0, "Wayland disconnected");
    } else
      wl_display_cancel_read(display);
    require(!(n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))),
            "Wayland connection lost");
    require(wl_display_dispatch_pending(display) >= 0,
            "Wayland protocol error");
  }
  ~Impl() {
    scene.reset();
    if (context != EGL_NO_CONTEXT) {
      eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
      windows.reset();
      resetCapture();
      if (program)
        glDeleteProgram(program);
      if (vao)
        glDeleteVertexArrays(1, &vao);
    }
    destroySurface();
    if (context != EGL_NO_CONTEXT)
      eglDestroyContext(egl, context);
    if (egl != EGL_NO_DISPLAY)
      eglTerminate(egl);
    if (gbm)
      gbm_device_destroy(gbm);
    if (drmFd >= 0)
      close(drmFd);
    for (auto &[id, o] : outputs)
      wl_output_release(o->wl);
    if (xdg)
      xdg_wm_base_destroy(xdg);
    if (dma)
      zwp_linux_dmabuf_v1_destroy(dma);
    if (copy)
      zwlr_screencopy_manager_v1_destroy(copy);
    if (shell)
      zwlr_layer_shell_v1_destroy(shell);
    if (shm)
      wl_shm_destroy(shm);
    if (compositor)
      wl_compositor_destroy(compositor);
    if (registry)
      wl_registry_destroy(registry);
    if (display)
      wl_display_disconnect(display);
  }
};
const wl_output_listener Display::Impl::outputListener = {
    [](void *d, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
       const char *make, const char *model, int32_t transform) {
      auto &o = *static_cast<Output *>(d);
      o.make = make;
      o.model = model;
      o.transform = transform;
    },
    [](void *d, wl_output *, uint32_t flags, int32_t width, int32_t height,
       int32_t) {
      if (flags & WL_OUTPUT_MODE_CURRENT) {
        auto &o = *static_cast<Output *>(d);
        o.width = width;
        o.height = height;
      }
    },
    [](void *, wl_output *) {},
    [](void *d, wl_output *, int32_t scale) {
      static_cast<Output *>(d)->scale = scale;
    },
    [](void *d, wl_output *, const char *n) {
      static_cast<Output *>(d)->name = n;
    },
    [](void *, wl_output *, const char *) {}};
const wl_registry_listener Display::Impl::registryListener = {
    [](void *d, wl_registry *r, uint32_t id, const char *name, uint32_t v) {
      auto &a = *static_cast<Impl *>(d);
#define BIND(proto, field, ver)                                                \
  if (std::strcmp(name, #proto) == 0) {                                        \
    a.field = static_cast<proto *>(wl_registry_bind(                           \
        r, id, &proto##_interface, std::min(v, uint32_t(ver))));               \
    return;                                                                    \
  }
      BIND(wl_compositor, compositor, 4)
      BIND(wl_shm, shm, 1)
      BIND(zwlr_layer_shell_v1, shell, 4)
      BIND(zwlr_screencopy_manager_v1, copy, 3)
      BIND(zwp_linux_dmabuf_v1, dma, 3)
      BIND(xdg_wm_base, xdg, 1)
#undef BIND
      if (std::strcmp(name, "wl_output") == 0 && v >= 4) {
        auto o = std::make_unique<Output>();
        o->owner = &a;
        o->id = id;
        o->wl = static_cast<wl_output *>(
            wl_registry_bind(r, id, &wl_output_interface, 4));
        wl_output_add_listener(o->wl, &outputListener, o.get());
        a.outputs[id] = std::move(o);
      }
    },
    [](void *d, wl_registry *, uint32_t id) {
      auto &a = *static_cast<Impl *>(d);
      auto i = a.outputs.find(id);
      if (i != a.outputs.end()) {
        i->second->alive = false;
        std::cerr << "output removed: " << i->second->name << "\n";
        if (a.target == i->second.get())
          a.closed = true;
      }
    }};
const zwp_linux_dmabuf_v1_listener Display::Impl::dmaListener = {
    [](void *, zwp_linux_dmabuf_v1 *, uint32_t) {},
    [](void *d, zwp_linux_dmabuf_v1 *, uint32_t f, uint32_t hi, uint32_t lo) {
      static_cast<Impl *>(d)->modifiers[f].push_back((uint64_t(hi) << 32) | lo);
    }};
const xdg_wm_base_listener Display::Impl::xdgListener = {
    [](void *, xdg_wm_base *x, uint32_t s) { xdg_wm_base_pong(x, s); }};
const zwlr_layer_surface_v1_listener Display::Impl::layerListener = {
    [](void *d, zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w,
       uint32_t h) {
      auto &a = *static_cast<Impl *>(d);
      zwlr_layer_surface_v1_ack_configure(s, serial);
      a.width = w ? int(w) : a.width;
      a.height = h ? int(h) : a.height;
      a.configured = true;
      if (a.window)
        wl_egl_window_resize(a.window, a.width, a.height, 0, 0);
    },
    [](void *d, zwlr_layer_surface_v1 *) {
      static_cast<Impl *>(d)->closed = true;
    }};
const xdg_surface_listener Display::Impl::xsListener = {
    [](void *d, xdg_surface *s, uint32_t serial) {
      xdg_surface_ack_configure(s, serial);
      static_cast<Impl *>(d)->configured = true;
    }};
const xdg_toplevel_listener Display::Impl::topListener = {
    [](void *d, xdg_toplevel *, int32_t w, int32_t h, wl_array *) {
      auto &a = *static_cast<Impl *>(d);
      if (w > 0)
        a.width = w;
      if (h > 0)
        a.height = h;
      if (a.window)
        wl_egl_window_resize(a.window, a.width, a.height, 0, 0);
    },
    [](void *d, xdg_toplevel *) { static_cast<Impl *>(d)->closed = true; },
    nullptr, nullptr};
const zwlr_screencopy_frame_v1_listener Display::Impl::frameListener = {
    [](void *d, zwlr_screencopy_frame_v1 *, uint32_t f, uint32_t w, uint32_t h,
       uint32_t s) {
      auto &a = *static_cast<Impl *>(d);
      a.shmFormat = f;
      a.sw = w;
      a.sh = h;
      a.stride = s;
    },
    [](void *d, zwlr_screencopy_frame_v1 *, uint32_t f) {
      auto &a = *static_cast<Impl *>(d);
      if (a.pending >= 0)
        a.slots[a.pending].flip = f & 1;
    },
    [](void *d, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {
      auto &a = *static_cast<Impl *>(d);
      a.ready();
    },
    [](void *d, zwlr_screencopy_frame_v1 *) {
      static_cast<Impl *>(d)->captureFailed();
    },
    [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t,
       uint32_t) {},
    [](void *d, zwlr_screencopy_frame_v1 *, uint32_t f, uint32_t w,
       uint32_t h) {
      auto &a = *static_cast<Impl *>(d);
      a.dmaFormat = f;
      a.dw = w;
      a.dh = h;
    },
    [](void *d, zwlr_screencopy_frame_v1 *) {
      auto &a = *static_cast<Impl *>(d);
      try {
        a.prepareBuffer();
      } catch (const std::exception &e) {
        a.error = e.what();
      }
    }};
const zwp_linux_buffer_params_v1_listener Display::Impl::paramsListener = {
    [](void *d, zwp_linux_buffer_params_v1 *params, wl_buffer *b) {
      auto &a = *static_cast<Impl *>(d);
      auto &s = a.slots[a.pending];
      s.buffer = b;
      s.waiting = false;
      zwp_linux_buffer_params_v1_destroy(params);
      s.params = nullptr;
      if (a.frame)
        zwlr_screencopy_frame_v1_copy(a.frame, b);
    },
    [](void *d, zwp_linux_buffer_params_v1 *params) {
      auto &a = *static_cast<Impl *>(d);
      auto &s = a.slots[a.pending];
      zwp_linux_buffer_params_v1_destroy(params);
      s.params = nullptr;
      s.waiting = false;
      a.captureFailed();
    }};
const wl_callback_listener Display::Impl::callbackListener = {
    [](void *d, wl_callback *cb, uint32_t) {
      auto &a = *static_cast<Impl *>(d);
      wl_callback_destroy(cb);
      a.callback = nullptr;
      a.framePermit = true;
    }};

Display::Display(const Config &c, const std::string &s, bool preview, bool flat)
    : p(std::make_unique<Impl>(c, s, preview, flat)) {
  p->init();
}
Display::~Display() = default;
void Display::dispatch(int ms) { p->dispatch(ms); }
void Display::render(Quat q) { p->render(q); }
void Display::maintain() { p->maintain(); }
bool Display::ready() const { return p->contentPresented; }
std::string Display::desktopGeneration() const {
  return p->contentPresented && p->scene
             ? p->scene->readyGeneration(p->windows->images())
             : std::string{};
}
void Display::setIpd(double millimeters) {
  require(std::isfinite(millimeters) && millimeters >= 45 && millimeters <= 80,
          "IPD must be between 45 and 80 mm");
  p->cfg.ipd_mm = millimeters;
  std::cerr << "IPD=" << millimeters << " mm\n";
}
std::string Display::statistics() const {
  return "rendered=" + std::to_string(p->rendered) +
         " captured=" + std::to_string(p->capturedFrames) +
         " capture_failed=" + std::to_string(p->failed) +
         " busy_buffer_drops=" + std::to_string(p->dropped) +
         (p->windows
              ? " window_frames=" + std::to_string(p->windows->frames()) +
                    " window_failed=" + std::to_string(p->windows->failures()) +
                    " scene=" + p->scene->status()
              : "");
}
} // namespace xr
