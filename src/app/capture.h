// Captures the selected real camera with Media Foundation and publishes NV12
// frames of the configured output size (at most 30 fps) into the shared
// frame channel. Runs on its own thread; recovers from unplug/busy/stall by
// itself and publishes a placeholder while no camera is delivering.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/shared_frame.h"
#include "common/strings.h"

namespace wcs {

struct CameraInfo {
    std::wstring name;
    std::wstring link;  // MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK
};

// Real cameras visible to Media Foundation (the DirectShow-only virtual
// cameras, including ours, are not listed).
std::vector<CameraInfo> listCameras();

struct NativeFormat {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    double fps;
};

// A camera mode chosen in the menu; fourcc 0 means automatic.
struct CameraMode {
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int fps = 0;

    bool isAuto() const { return fourcc == 0; }
    bool matches(const NativeFormat &format) const;
    std::wstring text() const;  // "NV12 1920x1080 30", "" for automatic
    static CameraMode parse(const std::wstring &text);
};

std::wstring fourccName(uint32_t fourcc);

// The native mode to request for `output` (index into `types`), see capture.cpp.
int chooseFormat(const std::vector<NativeFormat> &types, OutputSize output);

enum class CaptureState {
    Starting,
    Running,
    NoCamera,
    CameraBusy,
    ChannelBusy,  // another writer (second app copy, test tool) owns the channel
    Error,
};

struct CaptureStatus {
    CaptureState state = CaptureState::Starting;
    std::wstring camera;        // device name
    Str reason = Str::Count;    // Str::Count: none
    std::wstring detail;        // mode text when running, error code otherwise
};

class Capture {
public:
    // statusMessage is posted to `window` whenever status() changes.
    Capture(HWND window, UINT statusMessage);
    ~Capture();

    void start(const CameraInfo &camera, const CameraMode &mode, OutputSize output);
    void stop();

    // Each of these restarts capture with the new value.
    void select(const CameraInfo &camera);
    void setMode(const CameraMode &mode);
    void setOutputSize(OutputSize output);
    void restart();

    void devicesChanged();  // device arrival/removal hint

    CaptureStatus status();
    std::vector<NativeFormat> modes();  // of the camera opened last

private:
    class Reader;

    void run();
    bool runDevice(const CameraInfo &camera);
    void runPlaceholder(DWORD durationMs, Str subtitle);
    void setStatus(CaptureState state, const std::wstring &camera, Str reason,
                   const std::wstring &detail = {});
    CameraInfo pickCamera(const std::vector<CameraInfo> &cameras);

    HWND m_window;
    UINT m_statusMessage;
    std::thread m_thread;
    HANDLE m_stopEvent;
    HANDLE m_wakeEvent;  // restart / selection / device change
    std::atomic<bool> m_stopping {false};
    std::atomic<bool> m_restart {false};

    std::mutex m_mutex;  // guards the members below
    CaptureStatus m_status;
    CameraInfo m_selection;
    CameraMode m_mode;
    OutputSize m_output = kDefaultOutputSize;
    std::vector<NativeFormat> m_modes;

    FrameWriter m_writer;  // used by the capture thread, or by MF callbacks while a device runs
};

}  // namespace wcs
