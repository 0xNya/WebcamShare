#include "convert.h"

#include <windows.h>
#include <cstring>
#include <vector>

namespace wcs {

int bitsPerPixel(PixelFormat format)
{
    switch (format) {
    case PixelFormat::YUY2:
        return 16;
    case PixelFormat::NV12:
        return 12;
    case PixelFormat::RGB24:
        return 24;
    case PixelFormat::RGB32:
        return 32;
    default:
        return 0;
    }
}

static inline uint8_t clamp8(int value)
{
    return uint8_t(value < 0? 0: value > 255? 255: value);
}

// BT.601 limited range, 8-bit fixed point.
template<int BytesPerPixel>
static void nv12ToRgb(const uint8_t *nv12, uint32_t width, uint32_t height,
                      uint8_t *dst, int dstStride, bool bottomUp)
{
    const uint8_t *uvPlane = nv12 + size_t(width) * height;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *yRow = nv12 + size_t(y) * width;
        const uint8_t *uvRow = uvPlane + size_t(y / 2) * width;
        uint8_t *out = dst + (bottomUp? height - 1 - y: y) * size_t(dstStride);

        for (uint32_t x = 0; x < width; x += 2) {
            int d = uvRow[x] - 128;
            int e = uvRow[x + 1] - 128;
            int rv = 409 * e + 128;
            int guv = -100 * d - 208 * e + 128;
            int bu = 516 * d + 128;

            for (int i = 0; i < 2; i++) {
                int c = 298 * (yRow[x + i] - 16);
                out[0] = clamp8((c + bu) >> 8);
                out[1] = clamp8((c + guv) >> 8);
                out[2] = clamp8((c + rv) >> 8);

                if constexpr (BytesPerPixel == 4)
                    out[3] = 255;

                out += BytesPerPixel;
            }
        }
    }
}

void convertFromNv12(const uint8_t *nv12, uint32_t width, uint32_t height, PixelFormat format,
                     uint8_t *dst, int dstStride, bool bottomUp)
{
    switch (format) {
    case PixelFormat::NV12: {
        for (uint32_t y = 0; y < height * 3 / 2; y++)
            memcpy(dst + y * size_t(dstStride), nv12 + size_t(y) * width, width);

        break;
    }
    case PixelFormat::YUY2: {
        const uint8_t *uvPlane = nv12 + size_t(width) * height;

        for (uint32_t y = 0; y < height; y++) {
            const uint8_t *yRow = nv12 + size_t(y) * width;
            const uint8_t *uvRow = uvPlane + size_t(y / 2) * width;
            uint8_t *out = dst + y * size_t(dstStride);

            for (uint32_t x = 0; x < width; x += 2) {
                out[0] = yRow[x];
                out[1] = uvRow[x];
                out[2] = yRow[x + 1];
                out[3] = uvRow[x + 1];
                out += 4;
            }
        }

        break;
    }
    case PixelFormat::RGB24:
        nv12ToRgb<3>(nv12, width, height, dst, dstStride, bottomUp);

        break;
    case PixelFormat::RGB32:
        nv12ToRgb<4>(nv12, width, height, dst, dstStride, bottomUp);

        break;
    default:
        break;
    }
}

// Bilinear resize of one plane with `channels` interleaved samples per pixel.
static void scalePlane(const uint8_t *src, uint32_t srcWidth, uint32_t srcHeight,
                       uint8_t *dst, uint32_t dstWidth, uint32_t dstHeight, uint32_t channels)
{
    // 16.16 fixed point source positions of pixel centers.
    std::vector<uint32_t> xs(dstWidth);
    uint32_t xStep = uint32_t((uint64_t(srcWidth) << 16) / dstWidth);
    uint32_t yStep = uint32_t((uint64_t(srcHeight) << 16) / dstHeight);
    uint32_t maxX = (srcWidth - 1) << 16;
    uint32_t maxY = (srcHeight - 1) << 16;

    for (uint32_t x = 0; x < dstWidth; x++) {
        int64_t position = int64_t(x) * xStep + xStep / 2 - 0x8000;
        xs[x] = uint32_t(position < 0? 0: position > maxX? maxX: position);
    }

    size_t srcStride = size_t(srcWidth) * channels;

    for (uint32_t y = 0; y < dstHeight; y++) {
        int64_t position = int64_t(y) * yStep + yStep / 2 - 0x8000;
        uint32_t sy = uint32_t(position < 0? 0: position > maxY? maxY: position);
        uint32_t row = sy >> 16;
        uint32_t fy = (sy >> 8) & 0xFF;
        const uint8_t *top = src + row * srcStride;
        const uint8_t *bottom = row + 1 < srcHeight? top + srcStride: top;
        uint8_t *out = dst + size_t(y) * dstWidth * channels;

        for (uint32_t x = 0; x < dstWidth; x++) {
            uint32_t column = xs[x] >> 16;
            uint32_t fx = (xs[x] >> 8) & 0xFF;
            uint32_t next = column + 1 < srcWidth? column + 1: column;

            for (uint32_t c = 0; c < channels; c++) {
                uint32_t a = top[column * channels + c];
                uint32_t b = top[next * channels + c];
                uint32_t d = bottom[column * channels + c];
                uint32_t e = bottom[next * channels + c];
                uint32_t upper = a * (256 - fx) + b * fx;
                uint32_t lower = d * (256 - fx) + e * fx;
                out[x * channels + c] = uint8_t((upper * (256 - fy) + lower * fy + 32768) >> 16);
            }
        }
    }
}

void scaleNv12(const uint8_t *src, uint32_t srcWidth, uint32_t srcHeight,
               uint8_t *dst, uint32_t dstWidth, uint32_t dstHeight)
{
    scalePlane(src, srcWidth, srcHeight, dst, dstWidth, dstHeight, 1);
    scalePlane(src + size_t(srcWidth) * srcHeight, srcWidth / 2, srcHeight / 2,
               dst + size_t(dstWidth) * dstHeight, dstWidth / 2, dstHeight / 2, 2);
}

void renderPlaceholder(uint8_t *nv12, uint32_t width, uint32_t height,
                       const wchar_t *title, const wchar_t *subtitle)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = LONG(width);
    info.bmiHeader.biHeight = -LONG(height);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    // Y = 16 (black) is the fallback if GDI is unavailable.
    memset(nv12, 16, size_t(width) * height);
    memset(nv12 + size_t(width) * height, 128, size_t(width) * height / 2);

    if (!dc || !bitmap) {
        if (bitmap)
            DeleteObject(bitmap);

        if (dc)
            DeleteDC(dc);

        return;
    }

    auto oldBitmap = SelectObject(dc, bitmap);
    RECT all = {0, 0, LONG(width), LONG(height)};
    auto background = CreateSolidBrush(RGB(24, 24, 28));
    FillRect(dc, &all, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    // Text sizes were tuned for 720p; scale with the frame.
    auto scaled = [&](int value) { return int(value * int(height) / 720); };

    auto drawLine = [&](const wchar_t *text, int textHeight, COLORREF color, int top) {
        if (!text || !*text)
            return;

        auto font = CreateFontW(textHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto oldFont = SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT rect = {0, top, LONG(width), top + textHeight * 2};
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    };

    drawLine(title, scaled(56), RGB(230, 230, 235), int(height) / 2 - scaled(70));
    drawLine(subtitle, scaled(34), RGB(150, 150, 160), int(height) / 2 + scaled(10));
    GdiFlush();

    // Grey-only picture: luma from the green channel, neutral chroma.
    auto pixels = static_cast<const uint8_t *>(bits);

    for (size_t i = 0; i < size_t(width) * height; i++)
        nv12[i] = uint8_t(16 + pixels[i * 4 + 1] * 219 / 255);

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

}  // namespace wcs
