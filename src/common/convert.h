// NV12 (the shared-memory format) to the formats the virtual camera offers
// its clients, NV12 scaling and a placeholder frame renderer.
#pragma once

#include <cstdint>

namespace wcs {

enum class PixelFormat { YUY2, NV12, RGB24, RGB32, Count };

// Bits per pixel of the client-side format.
int bitsPerPixel(PixelFormat format);

// Converts a width x height NV12 frame (stride = width) to `format`.
// dstStride is in bytes; bottomUp flips rows (DIB-style RGB).
void convertFromNv12(const uint8_t *nv12, uint32_t width, uint32_t height, PixelFormat format,
                     uint8_t *dst, int dstStride, bool bottomUp);

// Bilinear NV12 resize (strides = widths). Sizes must be even.
void scaleNv12(const uint8_t *src, uint32_t srcWidth, uint32_t srcHeight,
               uint8_t *dst, uint32_t dstWidth, uint32_t dstHeight);

// Renders a dark frame with two centered text lines into an NV12 buffer.
void renderPlaceholder(uint8_t *nv12, uint32_t width, uint32_t height,
                       const wchar_t *title, const wchar_t *subtitle);

}  // namespace wcs
