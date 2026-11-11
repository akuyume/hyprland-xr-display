// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <cstring>
#include <wayland-client-protocol.h>

namespace xr {
struct ShmFormat {
  uint32_t pixelBytes;
  bool swapRB;
};

inline ShmFormat shmFormatInfo(uint32_t format) {
  // wl_shm's names describe bit order. GL reads bytes in memory order.
  switch (format) {
  case WL_SHM_FORMAT_RGB888:
    return {3, true};
  case WL_SHM_FORMAT_BGR888:
    return {3, false};
  case WL_SHM_FORMAT_ARGB8888:
  case WL_SHM_FORMAT_XRGB8888:
    return {4, true};
  case WL_SHM_FORMAT_ABGR8888:
  case WL_SHM_FORMAT_XBGR8888:
    return {4, false};
  default:
    return {0, false};
  }
}

inline void packShmRows(void *destination, const void *source,
                        uint32_t rowBytes, uint32_t height, uint32_t stride) {
  auto *out = static_cast<uint8_t *>(destination);
  auto *in = static_cast<const uint8_t *>(source);
  for (uint32_t row = 0; row < height; ++row) {
    std::memcpy(out, in, rowBytes);
    out += rowBytes;
    in += stride;
  }
}
} // namespace xr
