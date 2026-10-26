// Unit tests for the parts that do not need a camera or a DirectShow graph.
// Console exe, exits non-zero on failure.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "app/capture.h"
#include "app/settings.h"
#include "common/convert.h"
#include "common/shared_frame.h"
#include "common/strings.h"

using namespace wcs;

static int g_failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            g_failures++;                                                                \
        }                                                                                \
    } while (0)

constexpr uint32_t NV12 = 0x3231564E;
constexpr uint32_t YUY2 = 0x32595559;
constexpr uint32_t MJPG = 0x47504A4D;
constexpr OutputSize k720 = {1280, 720};
constexpr OutputSize k1080 = {1920, 1080};

static void testChooseFormat()
{
    // Typical webcam: YUY2 720p only at 10 fps, MJPEG 720p at 30.
    std::vector<NativeFormat> modes = {
        {YUY2, 640, 480, 30}, {YUY2, 1280, 720, 10}, {MJPG, 1920, 1080, 30},
        {MJPG, 1280, 720, 30}, {MJPG, 640, 480, 30},
    };
    CHECK(chooseFormat(modes, k720) == 3);
    CHECK(chooseFormat(modes, k1080) == 2);

    // Uncompressed 720p at full rate beats MJPEG (no decoding).
    modes.push_back({YUY2, 1280, 720, 30});
    CHECK(chooseFormat(modes, k720) == 5);
    modes.push_back({NV12, 1280, 720, 30});
    CHECK(chooseFormat(modes, k720) == 6);

    // 30 fps preferred over 60 at the same size.
    std::vector<NativeFormat> rates = {{MJPG, 1280, 720, 60}, {MJPG, 1280, 720, 30}};
    CHECK(chooseFormat(rates, k720) == 1);

    // No exact size: the smallest mode of the same aspect that covers it.
    std::vector<NativeFormat> big = {{MJPG, 3840, 2160, 30}, {MJPG, 1920, 1080, 30}, {MJPG, 1024, 576, 30}};
    CHECK(chooseFormat(big, k720) == 1);
    CHECK(chooseFormat(big, {640, 360}) == 2);

    // Only small modes: the largest.
    std::vector<NativeFormat> small = {{YUY2, 320, 240, 30}, {YUY2, 640, 480, 30}};
    CHECK(chooseFormat(small, k720) == 1);

    // Only slow modes: still pick something (second pass), highest rate.
    std::vector<NativeFormat> slow = {{YUY2, 1280, 720, 5}, {YUY2, 1280, 720, 10}};
    CHECK(chooseFormat(slow, k720) == 1);

    CHECK(chooseFormat({}, k720) == -1);
}

static void testCameraMode()
{
    CameraMode mode = {NV12, 1920, 1080, 60};
    CHECK(mode.text() == L"NV12 1920x1080 60");
    auto parsed = CameraMode::parse(mode.text());
    CHECK(parsed.fourcc == NV12 && parsed.width == 1920 && parsed.height == 1080 && parsed.fps == 60);
    CHECK(parsed.matches({NV12, 1920, 1080, 59.94}));
    CHECK(!parsed.matches({NV12, 1920, 1080, 30}));
    CHECK(CameraMode::parse(L"").isAuto());
    CHECK(CameraMode::parse(L"garbage").isAuto());
    CHECK(CameraMode {}.text().empty());
}

static void testJson()
{
    auto values = parseFlatJson(R"({"cameraName": "Razer Kiyo Pro", "cameraLink": "\\\\?\\usb#vid_1532", "x": true})");
    CHECK(values.has_value());
    CHECK((*values)[L"cameraName"] == L"Razer Kiyo Pro");
    CHECK((*values)[L"cameraLink"] == L"\\\\?\\usb#vid_1532");
    CHECK((*values)[L"x"] == L"true");

    // Round trip with non-ASCII and escapes.
    std::map<std::wstring, std::wstring> original = {{L"cameraName", L"Камера \"№1\"\t\\"}, {L"cameraLink", L""}};
    auto parsed = parseFlatJson(serializeFlatJson(original));
    CHECK(parsed.has_value() && *parsed == original);

    CHECK(parseFlatJson("{\"a\": \"\\u0416\"}").value()[L"a"] == L"Ж");
    CHECK(parseFlatJson("\xEF\xBB\xBF{}").has_value());
    CHECK(!parseFlatJson("{\"a\": ").has_value());
    CHECK(!parseFlatJson("{\"a\": \"b\"} trailing").has_value());
    CHECK(!parseFlatJson("{\"a\": \"\\uZZZZ\"}").has_value());
}

static void testConvert()
{
    const uint32_t width = 1280, height = 720;
    std::vector<uint8_t> nv12(nv12Size(width, height));
    // Mid grey: Y=126, U=V=128 -> RGB about 128.
    memset(nv12.data(), 126, width * height);
    memset(nv12.data() + width * height, 128, width * height / 2);
    nv12[0] = 235;  // top-left pixel white

    std::vector<uint8_t> yuy2(width * height * 2);
    convertFromNv12(nv12.data(), width, height, PixelFormat::YUY2, yuy2.data(), width * 2, false);
    CHECK(yuy2[0] == 235 && yuy2[1] == 128 && yuy2[2] == 126 && yuy2[3] == 128);

    std::vector<uint8_t> rgb(width * height * 3);
    convertFromNv12(nv12.data(), width, height, PixelFormat::RGB24, rgb.data(), width * 3, true);
    // Bottom-up: the top-left pixel is the first pixel of the last row.
    const uint8_t *topLeft = rgb.data() + (height - 1) * width * 3;
    CHECK(topLeft[0] == 255 && topLeft[1] == 255 && topLeft[2] == 255);
    CHECK(rgb[0] >= 127 && rgb[0] <= 129 && rgb[1] == rgb[0] && rgb[2] == rgb[0]);

    std::vector<uint8_t> rgb32(width * height * 4);
    convertFromNv12(nv12.data(), width, height, PixelFormat::RGB32, rgb32.data(), width * 4, false);
    CHECK(rgb32[0] == 255 && rgb32[3] == 255);

    // Saturated colours stay in range (red: Y=81 U=90 V=240).
    memset(nv12.data(), 81, width * height);

    for (uint32_t i = 0; i < width * height / 2; i += 2) {
        nv12[width * height + i] = 90;
        nv12[width * height + i + 1] = 240;
    }

    convertFromNv12(nv12.data(), width, height, PixelFormat::RGB24, rgb.data(), width * 3, false);
    CHECK(rgb[2] > 230 && rgb[1] < 20 && rgb[0] < 20);  // BGR order
}

static void testScale()
{
    // A flat frame stays flat; a left/right split keeps its sides.
    std::vector<uint8_t> src(nv12Size(1920, 1080));
    std::vector<uint8_t> dst(nv12Size(1280, 720));

    for (uint32_t y = 0; y < 1080; y++)
        for (uint32_t x = 0; x < 1920; x++)
            src[y * 1920 + x] = x < 960? 50: 200;

    memset(src.data() + 1920 * 1080, 128, 1920 * 1080 / 2);
    scaleNv12(src.data(), 1920, 1080, dst.data(), 1280, 720);
    CHECK(dst[0] == 50 && dst[1279] == 200 && dst[719 * 1280] == 50);
    CHECK(dst[1280 * 720] == 128 && dst.back() == 128);

    // Upscale does not read past the source.
    std::vector<uint8_t> big(nv12Size(1920, 1080));
    scaleNv12(dst.data(), 1280, 720, big.data(), 1920, 1080);
    CHECK(big[0] == 50 && big[1919] == 200);
}

static void testStrings()
{
    setRussian(false);
    CHECK(wcscmp(tr(Str::MenuExit), L"Exit") == 0);
    setRussian(true);
    CHECK(wcscmp(tr(Str::MenuExit), L"Выход") == 0);
    setRussian(false);
}

static void testChannel()
{
    FrameWriter writer;

    if (!writer.open()) {
        printf("skip channel test: channel busy (app or patternfeed running)\n");

        return;
    }

    FrameReader reader;
    CHECK(reader.tryOpen());
    std::vector<uint8_t> frame(kMaxFrameSize);
    FrameInfo info;

    for (int n = 0; n < 5; n++) {
        bool big = n == 4;
        uint32_t width = big? 1920: 1280, height = big? 1080: 720;
        auto data = writer.beginFrame(width, height);
        memset(data, 10 + n, nv12Size(width, height));
        writer.publish(big? FramePlaceholder: FrameLive);
    }

    CHECK(writer.beginFrame(1921, 1080) == nullptr);  // too large
    CHECK(writer.beginFrame(1279, 720) == nullptr);   // odd

    CHECK(reader.writerAlive());
    CHECK(reader.readLatest(frame.data(), &info));
    CHECK(info.width == 1920 && info.height == 1080 && info.kind == FramePlaceholder);
    CHECK(frame[0] == 14 && frame[nv12Size(1920, 1080) - 1] == 14);

    // No newer frame: still true, buffer untouched.
    frame[0] = 0;
    LONG64 before = info.seq;
    CHECK(reader.readLatest(frame.data(), &info) && info.seq == before && frame[0] == 0);

    // A writer in another process is refused while this one is alive
    // (patternfeed exits with 1 when it cannot open the channel).
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring feeder = path;
    feeder = feeder.substr(0, feeder.rfind(L'\\')) + L"\\patternfeed.exe";
    std::wstring commandLine = L"\"" + feeder + L"\" 1";
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};

    if (CreateProcessW(feeder.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process)) {
        // Keep the heartbeat fresh while the other process starts.
        for (int i = 0; i < 50 && WaitForSingleObject(process.hProcess, 20) == WAIT_TIMEOUT; i++) {
            writer.beginFrame(1280, 720);
            writer.publish(FrameLive);
        }

        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        CHECK(code == 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        printf("skip second-writer test: %ls not found\n", feeder.c_str());
    }

    writer.close();
    CHECK(!reader.writerAlive());
}

int main()
{
    testChooseFormat();
    testCameraMode();
    testJson();
    testConvert();
    testScale();
    testStrings();
    testChannel();

    if (g_failures)
        printf("%d check(s) failed\n", g_failures);
    else
        printf("all tests passed\n");

    return g_failures? 1: 0;
}
