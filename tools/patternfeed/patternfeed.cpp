// patternfeed: writes a moving test pattern into the WebcamShare frame
// channel at 30 fps, standing in for the app while testing the filter.
//   patternfeed [seconds]   (0 or nothing = until Ctrl+C)
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/common/shared_frame.h"

using namespace wcs;

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

static volatile bool g_running = true;

static BOOL WINAPI onCtrl(DWORD)
{
    g_running = false;

    return TRUE;
}

// Colour bars that scroll horizontally plus a bouncing white square.
static void drawPattern(uint8_t *nv12, uint64_t frame)
{
    static const uint8_t bars[8][3] = {  // Y, U, V
        {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
        {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128},
    };
    uint8_t *uv = nv12 + kWidth * kHeight;
    uint32_t shift = uint32_t(frame * 8 % kWidth);

    for (uint32_t y = 0; y < kHeight; y++) {
        uint8_t *row = nv12 + y * kWidth;

        for (uint32_t x = 0; x < kWidth; x++)
            row[x] = bars[((x + shift) % kWidth) * 8 / kWidth][0];
    }

    for (uint32_t y = 0; y < kHeight / 2; y++) {
        uint8_t *row = uv + y * kWidth;

        for (uint32_t x = 0; x < kWidth; x += 2) {
            auto &bar = bars[((x + shift) % kWidth) * 8 / kWidth];
            row[x] = bar[1];
            row[x + 1] = bar[2];
        }
    }

    const uint32_t box = 120;
    uint32_t period = (kHeight - box) * 2;
    uint32_t pos = uint32_t(frame * 6 % period);
    uint32_t top = pos < kHeight - box? pos: period - pos;
    uint32_t left = uint32_t(frame * 10 % (kWidth - box));

    for (uint32_t y = top; y < top + box; y++)
        memset(nv12 + y * kWidth + left, 235, box);

    for (uint32_t y = top / 2; y < (top + box) / 2; y++)
        memset(uv + y * kWidth + (left & ~1u), 128, box);
}

int wmain(int argc, wchar_t **argv)
{
    int seconds = argc > 1? _wtoi(argv[1]): 0;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    FrameWriter writer;

    if (!writer.open()) {
        DWORD error = GetLastError();
        printf(error == ERROR_BUSY? "another writer (WebcamShare or patternfeed) is running\n":
                                    "cannot open the frame channel: %lu\n", error);

        return 1;
    }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    printf("streaming test pattern%s\n", seconds? "": " (Ctrl+C to stop)");

    for (uint64_t frame = 0; g_running; frame++) {
        drawPattern(writer.beginFrame(kWidth, kHeight), frame);
        writer.publish(FrameLive);

        if (seconds && frame + 1 >= uint64_t(seconds) * kFps)
            break;

        // Sleep until the next frame's deadline (absolute pacing, no drift).
        QueryPerformanceCounter(&now);
        int64_t deadline = start.QuadPart + int64_t(frame + 1) * frequency.QuadPart / kFps;
        int64_t wait100ns = (deadline - now.QuadPart) * 10000000 / frequency.QuadPart;

        if (wait100ns > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -wait100ns;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
    }

    CloseHandle(timer);
    writer.close();
    printf("stopped\n");

    return 0;
}
