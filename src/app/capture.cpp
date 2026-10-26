#include "capture.h"
#include "log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "common/convert.h"

// Last: initguid turns the DEVPKEY declarations into definitions.
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

using Microsoft::WRL::ComPtr;

namespace wcs {

namespace {

constexpr DWORD kVideoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kRetryMs = 3000;           // camera missing/busy: retry period
constexpr ULONGLONG kFirstFrameMs = 8000;  // camera opened but silent
constexpr ULONGLONG kStallMs = 3000;       // frames stopped arriving
constexpr ULONGLONG kStatsPeriodMs = 60000;

std::wstring formatName(const NativeFormat &format)
{
    wchar_t text[96];
    swprintf_s(text, L"%s %ux%u %.0f fps", fourccName(format.fourcc).c_str(),
               format.width, format.height, format.fps);

    return text;
}

bool isBusyError(HRESULT hr)
{
    return hr == MF_E_VIDEO_DEVICE_LOCKED
           || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
           || hr == MF_E_HW_MFT_FAILED_START_STREAMING
           || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
           || hr == HRESULT_FROM_WIN32(ERROR_BUSY);
}

bool isGoneError(HRESULT hr)
{
    return hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
           || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)
           || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
           || hr == MF_E_SHUTDOWN;
}

// Copies a width x height NV12 picture from a (possibly padded) buffer.
bool copyNv12(const uint8_t *src, LONG pitch, DWORD length, uint32_t width, uint32_t height,
              uint8_t *dst)
{
    if (!src || pitch < LONG(width))
        return false;

    // The chroma plane follows the (maybe height-padded) luma plane.
    DWORD rows = height;

    if (length >= DWORD(pitch) * height * 3 / 2)
        rows = std::max<DWORD>(height, length / DWORD(pitch) * 2 / 3);

    if (pitch == LONG(width) && rows == height) {
        memcpy(dst, src, nv12Size(width, height));

        return true;
    }

    for (uint32_t y = 0; y < height; y++)
        memcpy(dst + size_t(y) * width, src + size_t(y) * size_t(pitch), width);

    const uint8_t *uv = src + size_t(pitch) * rows;
    uint8_t *uvDst = dst + size_t(width) * height;

    for (uint32_t y = 0; y < height / 2; y++)
        memcpy(uvDst + size_t(y) * width, uv + size_t(y) * size_t(pitch), width);

    return true;
}

// MF reports the interface name, often a generic "USB Video Device"; the
// device node usually has the real product name ("Razer Kiyo Pro").
std::wstring deviceFriendlyName(const std::wstring &link)
{
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    wchar_t name[256] = {};
    ULONG size = sizeof(instanceId);
    DEVPROPTYPE type = 0;
    DEVINST node = 0;

    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instanceId), &size, 0) != CR_SUCCESS
        || CM_Locate_DevNodeW(&node, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return {};

    // FriendlyName is often unset; DEVPKEY_NAME is what Device Manager shows.
    for (auto key: {&DEVPKEY_Device_FriendlyName, &DEVPKEY_NAME}) {
        size = sizeof(name);

        if (CM_Get_DevNode_PropertyW(node, key, &type, reinterpret_cast<PBYTE>(name), &size, 0)
                == CR_SUCCESS
            && type == DEVPROP_TYPE_STRING && name[0])
            return name;
    }

    return {};
}

// "YUY2 1280x720 @60/30/24" per format and size, in the camera's order.
std::wstring describeModes(const std::vector<NativeFormat> &formats)
{
    std::vector<std::pair<std::wstring, std::vector<int>>> groups;

    for (auto &f: formats) {
        wchar_t key[64];
        swprintf_s(key, L"%s %ux%u", fourccName(f.fourcc).c_str(), f.width, f.height);
        int fps = int(f.fps + 0.5);
        auto it = std::find_if(groups.begin(), groups.end(), [&](auto &g) { return g.first == key; });

        if (it == groups.end())
            groups.push_back({key, {fps}});
        else if (std::find(it->second.begin(), it->second.end(), fps) == it->second.end())
            it->second.push_back(fps);
    }

    std::wstring text;

    for (auto &[key, rates]: groups) {
        text += (text.empty()? L"": L", ") + key + L" @";

        for (size_t i = 0; i < rates.size(); i++)
            text += (i? L"/": L"") + std::to_wstring(rates[i]);
    }

    return text;
}

std::wstring hresultText(HRESULT hr)
{
    wchar_t code[16];
    swprintf_s(code, L"0x%08lX", ULONG(hr));

    return code;
}

}  // namespace

std::wstring fourccName(uint32_t fourcc)
{
    if (fourcc == MFVideoFormat_RGB32.Data1)
        return L"RGB32";

    if (fourcc == MFVideoFormat_RGB24.Data1)
        return L"RGB24";

    wchar_t name[5] = {};

    for (int i = 0; i < 4; i++) {
        auto c = wchar_t((fourcc >> (8 * i)) & 0xFF);
        name[i] = c >= 32 && c < 127? c: L'?';
    }

    return name;
}

bool CameraMode::matches(const NativeFormat &format) const
{
    return format.fourcc == fourcc && format.width == width && format.height == height
           && std::fabs(format.fps - fps) < 1;
}

std::wstring CameraMode::text() const
{
    if (isAuto())
        return {};

    wchar_t text[64];
    swprintf_s(text, L"%s %ux%u %d", fourccName(fourcc).c_str(), width, height, fps);

    return text;
}

CameraMode CameraMode::parse(const std::wstring &text)
{
    wchar_t name[5] = {};
    CameraMode mode;

    if (swscanf_s(text.c_str(), L"%4s %ux%u %d", name, unsigned(_countof(name)), &mode.width,
                  &mode.height, &mode.fps) != 4 || wcslen(name) != 4)
        return {};

    for (int i = 0; i < 4; i++)
        mode.fourcc |= uint32_t(name[i] & 0xFF) << (8 * i);

    return mode;
}

// Ranking: resolution closest to the output size without upscaling, then a
// frame rate of 30 (or 60, halved cleanly), then the cheapest pixel format
// to turn into NV12. Uncompressed modes below 29 fps are ignored: many
// webcams list YUY2 720p at 5-10 fps only.
int chooseFormat(const std::vector<NativeFormat> &types, OutputSize output)
{
    auto subtypeRank = [](uint32_t fourcc) {
        if (fourcc == MFVideoFormat_NV12.Data1)
            return 0;

        if (fourcc == MFVideoFormat_YUY2.Data1)
            return 1;

        if (fourcc == MFVideoFormat_MJPG.Data1)
            return 2;

        if (fourcc == MFVideoFormat_H264.Data1 || fourcc == MFVideoFormat_HEVC.Data1)
            return 9;  // works through MF decoders, but last resort

        return 5;
    };

    int best = -1;
    std::tuple<int, int, int, uint64_t> bestKey;

    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (size_t i = 0; i < types.size(); i++) {
            auto &t = types[i];

            if (!t.width || !t.height)
                continue;

            // Second pass: anything at all, highest frame rate first.
            if (pass == 0 && t.fps < 29)
                continue;

            bool covers = t.width >= output.width && t.height >= output.height;
            bool sameAspect = uint64_t(t.width) * output.height == uint64_t(t.height) * output.width;
            int resolutionClass = t.width == output.width && t.height == output.height? 0:
                                  covers && sameAspect? 1:
                                  covers? 2: 3;
            uint64_t area = uint64_t(t.width) * t.height;
            // Prefer the smallest frame that covers the output, else the largest.
            uint64_t areaKey = resolutionClass == 3? UINT64_MAX - area: area;
            int fpsClass = pass == 1? int(1000 - std::min(t.fps, 999.0)):
                           std::fabs(t.fps - 30) < 1? 0:
                           std::fabs(t.fps - 60) < 1? 1: 2;
            auto key = std::make_tuple(resolutionClass, fpsClass, subtypeRank(t.fourcc), areaKey);

            if (best < 0 || key < bestKey) {
                best = int(i);
                bestKey = key;
            }
        }
    }

    return best;
}

std::vector<CameraInfo> listCameras()
{
    std::vector<CameraInfo> cameras;
    ComPtr<IMFAttributes> attributes;

    if (FAILED(MFCreateAttributes(&attributes, 1)))
        return cameras;

    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate **devices = nullptr;
    UINT32 count = 0;

    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)))
        return cameras;

    for (UINT32 i = 0; i < count; i++) {
        WCHAR *name = nullptr;
        WCHAR *link = nullptr;
        UINT32 length = 0;

        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))
            && SUCCEEDED(devices[i]->GetAllocatedString(
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &length))) {
            auto productName = deviceFriendlyName(link);
            cameras.push_back({productName.empty()? name: productName, link});
        }

        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devices[i]->Release();
    }

    CoTaskMemFree(devices);

    return cameras;
}

// --- Reader: async source reader callback -----------------------------------

class Capture::Reader final: public IMFSourceReaderCallback {
public:
    Reader(FrameWriter *writer, OutputSize output, double nativeFps):
        m_writer(writer),
        m_output(output)
    {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_flushed = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // Above ~31 fps keep every n-th frame by timestamp (60 -> 30).
        if (nativeFps > 31)
            m_minInterval = kFrameDuration * 9 / 10;
    }

    ~Reader()
    {
        CloseHandle(m_event);
        CloseHandle(m_flushed);
    }

    void attach(IMFSourceReader *reader) { m_reader = reader; }
    HANDLE event() const { return m_event; }
    HRESULT error() const { return m_error.load(); }
    ULONGLONG lastFrameTick() const { return m_lastFrameTick.load(); }
    uint64_t frames() const { return m_frames.load(); }
    uint64_t dropped() const { return m_dropped.load(); }

    HRESULT requestSample()
    {
        return m_reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
    }

    // Stops issuing reads and waits until the reader has no request pending.
    void shutdown()
    {
        m_stopping = true;
        ResetEvent(m_flushed);

        if (SUCCEEDED(m_reader->Flush(kVideoStream)))
            WaitForSingleObject(m_flushed, 2000);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback *>(this);
            AddRef();

            return S_OK;
        }

        *ppv = nullptr;

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG ref = --m_refCount;

        if (!ref)
            delete this;

        return ref;
    }

    // IMFSourceReaderCallback
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD, DWORD flags,
                                           LONGLONG timestamp, IMFSample *sample) override
    {
        if (FAILED(status) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            fail(FAILED(status)? status: MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED);

            return S_OK;
        }

        if (sample)
            publish(sample, timestamp);

        if (!m_stopping) {
            HRESULT hr = requestSample();

            if (FAILED(hr))
                fail(hr);
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override
    {
        SetEvent(m_flushed);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent *) override
    {
        return S_OK;
    }

private:
    void fail(HRESULT hr)
    {
        HRESULT expected = S_OK;
        m_error.compare_exchange_strong(expected, hr);
        SetEvent(m_event);
    }

    void publish(IMFSample *sample, LONGLONG timestamp)
    {
        if (m_minInterval && m_lastTimestamp >= 0 && timestamp - m_lastTimestamp < m_minInterval) {
            m_dropped++;

            return;
        }

        ComPtr<IMFMediaBuffer> buffer;

        if (FAILED(sample->GetBufferByIndex(0, &buffer)))
            return;

        uint32_t width = m_output.width;
        uint32_t height = m_output.height;
        uint8_t *frame = m_writer->beginFrame(width, height);

        if (!frame)
            return;

        bool copied = false;
        ComPtr<IMF2DBuffer> buffer2d;

        if (SUCCEEDED(buffer.As(&buffer2d))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;

            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                DWORD length = 0;
                buffer2d->GetContiguousLength(&length);
                copied = copyNv12(scan0, pitch, length, width, height, frame);
                buffer2d->Unlock2D();
            }
        }

        if (!copied) {
            BYTE *data = nullptr;
            DWORD maxLength = 0;
            DWORD length = 0;

            if (SUCCEEDED(buffer->Lock(&data, &maxLength, &length))) {
                copied = length >= nv12Size(width, height)
                         && copyNv12(data, LONG(width), length, width, height, frame);
                buffer->Unlock();
            }
        }

        if (!copied)
            return;

        m_writer->publish(FrameLive);
        m_lastTimestamp = timestamp;
        m_lastFrameTick = GetTickCount64();

        if (m_frames++ == 0)
            SetEvent(m_event);  // first frame: lets the owner report "running"
    }

    std::atomic<ULONG> m_refCount {1};
    IMFSourceReader *m_reader = nullptr;  // not owned: the reader owns us
    FrameWriter *m_writer;
    OutputSize m_output;
    HANDLE m_event;
    HANDLE m_flushed;
    std::atomic<bool> m_stopping {false};
    std::atomic<HRESULT> m_error {S_OK};
    std::atomic<ULONGLONG> m_lastFrameTick {0};
    std::atomic<uint64_t> m_frames {0};
    std::atomic<uint64_t> m_dropped {0};
    LONGLONG m_minInterval = 0;
    LONGLONG m_lastTimestamp = -1;
};

// --- Capture -----------------------------------------------------------------

Capture::Capture(HWND window, UINT statusMessage):
    m_window(window),
    m_statusMessage(statusMessage)
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Capture::~Capture()
{
    stop();
    CloseHandle(m_stopEvent);
    CloseHandle(m_wakeEvent);
}

void Capture::start(const CameraInfo &camera, const CameraMode &mode, OutputSize output)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_selection = camera;
        m_mode = mode;
        m_output = output;
    }

    m_stopping = false;
    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { run(); });
}

void Capture::stop()
{
    if (!m_thread.joinable())
        return;

    m_stopping = true;
    SetEvent(m_stopEvent);
    m_thread.join();
}

void Capture::select(const CameraInfo &camera)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_selection = camera;
        m_mode = {};  // modes belong to a camera
        m_modes.clear();
    }

    restart();
}

void Capture::setMode(const CameraMode &mode)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mode = mode;
    }

    restart();
}

void Capture::setOutputSize(OutputSize output)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_output = output;
    }

    restart();
}

void Capture::restart()
{
    m_restart = true;
    SetEvent(m_wakeEvent);
}

void Capture::devicesChanged()
{
    SetEvent(m_wakeEvent);
}

CaptureStatus Capture::status()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_status;
}

std::vector<NativeFormat> Capture::modes()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_modes;
}

void Capture::setStatus(CaptureState state, const std::wstring &camera, Str reason,
                        const std::wstring &detail)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_status.state == state && m_status.camera == camera && m_status.reason == reason
            && m_status.detail == detail)
            return;

        m_status = {state, camera, reason, detail};
    }

    PostMessageW(m_window, m_statusMessage, 0, 0);
}

CameraInfo Capture::pickCamera(const std::vector<CameraInfo> &cameras)
{
    CameraInfo selection;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        selection = m_selection;
    }

    if (selection.link.empty() && selection.name.empty())
        return cameras.empty()? CameraInfo {}: cameras.front();

    for (auto &camera: cameras)
        if (_wcsicmp(camera.link.c_str(), selection.link.c_str()) == 0)
            return camera;

    // The link changes when the camera moves to another USB port.
    for (auto &camera: cameras)
        if (camera.name == selection.name)
            return camera;

    return {};
}

void Capture::run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    while (!m_stopping) {
        if (!m_writer.isOpen() && !m_writer.open()) {
            bool busy = GetLastError() == ERROR_BUSY;
            setStatus(busy? CaptureState::ChannelBusy: CaptureState::Error, L"",
                      busy? Str::DetailChannelBusy: Str::DetailChannelError);
            WaitForSingleObject(m_stopEvent, kRetryMs);

            continue;
        }

        m_restart = false;
        auto camera = pickCamera(listCameras());

        if (camera.link.empty()) {
            std::wstring wanted;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                wanted = m_selection.name;
            }

            setStatus(CaptureState::NoCamera, wanted, Str::Count);
            runPlaceholder(kRetryMs, Str::PlaceholderNoCamera);

            continue;
        }

        runDevice(camera);

        if (m_stopping || m_restart)
            continue;

        auto state = status().state;
        runPlaceholder(kRetryMs, state == CaptureState::CameraBusy? Str::PlaceholderBusy:
                                 state == CaptureState::NoCamera? Str::PlaceholderUnplugged:
                                                                  Str::PlaceholderNoSignal);
    }

    m_writer.close();
    MFShutdown();
    CoUninitialize();
}

// Publishes a placeholder (10 fps; the filter repeats frames in between)
// until the period ends, a wake-up arrives or the app stops.
void Capture::runPlaceholder(DWORD durationMs, Str subtitle)
{
    OutputSize output;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        output = m_output;
    }

    // Cached per text, size and language.
    static std::map<std::wstring, std::vector<uint8_t>> cache;
    auto key = std::wstring(tr(subtitle)) + L"|" + std::to_wstring(output.width);
    auto &frame = cache[key];

    if (frame.empty()) {
        frame.resize(nv12Size(output.width, output.height));
        renderPlaceholder(frame.data(), output.width, output.height, L"WebcamShare", tr(subtitle));
    }

    auto start = GetTickCount64();
    HANDLE handles[] = {m_stopEvent, m_wakeEvent};

    while (GetTickCount64() - start < durationMs) {
        if (auto data = m_writer.beginFrame(output.width, output.height)) {
            memcpy(data, frame.data(), frame.size());
            m_writer.publish(FramePlaceholder);
        }

        if (WaitForMultipleObjects(2, handles, FALSE, 100) != WAIT_TIMEOUT)
            return;
    }
}

bool Capture::runDevice(const CameraInfo &camera)
{
    CameraMode wantedMode;
    OutputSize output;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wantedMode = m_mode;
        output = m_output;
    }

    setStatus(CaptureState::Starting, camera.name, Str::DetailOpening);
    logInfo(L"opening camera \"%s\" (%s)", camera.name.c_str(), camera.link.c_str());

    auto failed = [&](HRESULT hr, const wchar_t *step) {
        logWarning(L"%s failed: 0x%08lX", step, ULONG(hr));

        if (isBusyError(hr))
            setStatus(CaptureState::CameraBusy, camera.name, Str::DetailBusy);
        else if (hr == E_ACCESSDENIED)
            setStatus(CaptureState::Error, camera.name, Str::DetailAccessDenied);
        else if (isGoneError(hr))
            setStatus(CaptureState::NoCamera, camera.name, Str::DetailUnplugged);
        else
            setStatus(CaptureState::Error, camera.name, Str::Count,
                      std::wstring(step) + L": " + hresultText(hr));

        return false;
    };

    ComPtr<IMFAttributes> deviceAttributes;
    MFCreateAttributes(&deviceAttributes, 2);
    deviceAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    deviceAttributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                camera.link.c_str());
    ComPtr<IMFMediaSource> source;
    HRESULT hr = MFCreateDeviceSource(deviceAttributes.Get(), &source);

    if (FAILED(hr))
        return failed(hr, L"MFCreateDeviceSource");

    std::vector<NativeFormat> formats;
    std::vector<ComPtr<IMFMediaType>> nativeTypes;
    ComPtr<IMFPresentationDescriptor> descriptor;
    hr = source->CreatePresentationDescriptor(&descriptor);

    if (SUCCEEDED(hr)) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        ComPtr<IMFMediaTypeHandler> handler;
        DWORD count = 0;

        if (SUCCEEDED(descriptor->GetStreamDescriptorByIndex(0, &selected, &stream))
            && SUCCEEDED(stream->GetMediaTypeHandler(&handler))
            && SUCCEEDED(handler->GetMediaTypeCount(&count))) {
            for (DWORD i = 0; i < count; i++) {
                ComPtr<IMFMediaType> type;

                if (FAILED(handler->GetMediaTypeByIndex(i, &type)))
                    continue;

                GUID subtype = GUID_NULL;
                UINT32 width = 0, height = 0, num = 0, den = 1;
                type->GetGUID(MF_MT_SUBTYPE, &subtype);
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
                MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
                formats.push_back({subtype.Data1, width, height, den? double(num) / den: 0});
                nativeTypes.push_back(type);
            }
        }
    }

    if (formats.empty()) {
        source->Shutdown();

        return failed(FAILED(hr)? hr: MF_E_INVALIDMEDIATYPE, L"enumerating formats");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_modes = formats;
    }

    logInfo(L"camera modes: %s", describeModes(formats).c_str());

    // The mode chosen in the menu, if this camera has it; otherwise automatic.
    int choice = -1;
    bool modeMissing = false;

    if (!wantedMode.isAuto()) {
        for (size_t i = 0; i < formats.size() && choice < 0; i++)
            if (wantedMode.matches(formats[i]))
                choice = int(i);

        modeMissing = choice < 0;

        if (modeMissing)
            logWarning(L"camera mode %s is not available, using automatic", wantedMode.text().c_str());
    }

    if (choice < 0)
        choice = chooseFormat(formats, output);

    if (choice < 0) {
        source->Shutdown();
        setStatus(CaptureState::Error, camera.name, Str::DetailNoMode);

        return false;
    }

    auto &native = formats[size_t(choice)];
    logInfo(L"selected mode: %s, output %ux%u", formatName(native).c_str(), output.width, output.height);

    auto reader = new Reader(&m_writer, output, native.fps);
    ComPtr<IMFAttributes> readerAttributes;
    MFCreateAttributes(&readerAttributes, 4);
    readerAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, reader);
    readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    readerAttributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    ComPtr<IMFSourceReader> sourceReader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttributes.Get(), &sourceReader);
    readerAttributes.Reset();

    if (FAILED(hr)) {
        reader->Release();
        source->Shutdown();

        return failed(hr, L"MFCreateSourceReaderFromMediaSource");
    }

    reader->attach(sourceReader.Get());

    // Native mode, then NV12 at the output size out (the reader inserts the
    // MJPEG decoder and, if the size differs, the video processor).
    hr = sourceReader->SetCurrentMediaType(kVideoStream, nullptr, nativeTypes[size_t(choice)].Get());

    ComPtr<IMFMediaType> outputType;

    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&outputType);

    if (SUCCEEDED(hr)) {
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, output.width, output.height);
        MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = sourceReader->SetCurrentMediaType(kVideoStream, nullptr, outputType.Get());
    }

    if (SUCCEEDED(hr))
        hr = reader->requestSample();

    if (FAILED(hr)) {
        reader->Release();
        sourceReader.Reset();
        source->Shutdown();

        return failed(hr, L"configuring the reader");
    }

    wchar_t outputText[32];
    swprintf_s(outputText, L"%ux%u", output.width, output.height);
    bool converted = native.fourcc != MFVideoFormat_NV12.Data1
                     || native.width != output.width || native.height != output.height;
    auto modeText = formatName(native) + (converted? L" → NV12 " + std::wstring(outputText): L"");
    auto opened = GetTickCount64();
    auto statsAt = opened + kStatsPeriodMs;
    uint64_t statsFrames = 0;
    uint64_t statsDropped = 0;
    bool running = false;
    HANDLE handles[] = {m_stopEvent, m_wakeEvent, reader->event()};

    for (;;) {
        DWORD wait = WaitForMultipleObjects(3, handles, FALSE, 500);
        auto now = GetTickCount64();

        if (wait == WAIT_OBJECT_0 || m_stopping)
            break;

        if (wait == WAIT_OBJECT_0 + 1) {
            if (m_restart) {
                logInfo(L"restarting capture");

                break;
            }

            continue;  // device-change hint: our device still delivers
        }

        if (FAILED(reader->error())) {
            failed(reader->error(), L"reading frames");

            break;
        }

        if (!running && reader->frames() > 0) {
            running = true;
            logInfo(L"capture running: %s", modeText.c_str());
            setStatus(CaptureState::Running, camera.name,
                      modeMissing? Str::DetailModeMissing: Str::Count, modeText);
        }

        if (!running && now - opened > kFirstFrameMs) {
            logWarning(L"camera opened but sent no frames");
            setStatus(CaptureState::CameraBusy, camera.name, Str::DetailNoFrames);

            break;
        }

        if (running && now - reader->lastFrameTick() > kStallMs) {
            logWarning(L"no frames for %llu ms, reopening", now - reader->lastFrameTick());
            setStatus(CaptureState::NoCamera, camera.name, Str::DetailStalled);

            break;
        }

        if (now >= statsAt) {
            auto frames = reader->frames();
            auto dropped = reader->dropped();
            logInfo(L"stats: %.1f fps published, %llu frames dropped (rate limit)",
                    double(frames - statsFrames) * 1000 / double(kStatsPeriodMs), dropped - statsDropped);
            statsFrames = frames;
            statsDropped = dropped;
            statsAt = now + kStatsPeriodMs;
        }
    }

    reader->shutdown();
    sourceReader.Reset();
    reader->Release();
    source->Shutdown();
    logInfo(L"camera closed");

    return true;
}

}  // namespace wcs
