// DirectShow source filter of the WebcamShare virtual camera.
//
// One output pin (category PIN_CATEGORY_CAPTURE) offering the configured
// output size (HKCU\Software\WebcamShare, read when the filter is created)
// at 30 fps in YUY2, NV12, RGB24 and RGB32. A streaming thread reads NV12
// frames from the shared-memory channel written by the app, scales them if
// the app now sends another size, and converts them to the format the client
// connected with; without a live writer it sends a placeholder.
#pragma once

#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "../common/config.h"
#include "../common/convert.h"

namespace wcs {

extern std::atomic<long> g_objectCount;
extern const CLSID CLSID_WebcamShareCam;
// Kept as is when the output size changes: apps remember cameras by name.
constexpr wchar_t kFilterName[] = L"WebcamShare 720p";
constexpr wchar_t kPinName[] = L"Capture";

void freeMediaType(AM_MEDIA_TYPE &mt);
void deleteMediaType(AM_MEDIA_TYPE *mt);
HRESULT copyMediaType(AM_MEDIA_TYPE &dst, const AM_MEDIA_TYPE &src);

class VCamFilter;

class VCamPin final: public IPin,
                     public IAMStreamConfig,
                     public IKsPropertySet,
                     public IAMPushSource {
public:
    VCamPin(VCamFilter *filter, uint32_t width, uint32_t height);
    ~VCamPin();

    // Filter state transitions, called with the filter lock held.
    HRESULT active();
    void inactive();

    // Media type `index` of the formats currently offered.
    HRESULT mediaType(int index, AM_MEDIA_TYPE *mt);

    // IUnknown (lifetime is the filter's)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IPin
    HRESULT STDMETHODCALLTYPE Connect(IPin *receivePin, const AM_MEDIA_TYPE *pmt) override;
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *pmt) override;
    HRESULT STDMETHODCALLTYPE Disconnect() override;
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin **pin) override;
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO *info) override;
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION *dir) override;
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR *id) override;
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE *pmt) override;
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes **ppEnum) override;
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin **pins, ULONG *count) override;
    HRESULT STDMETHODCALLTYPE EndOfStream() override;
    HRESULT STDMETHODCALLTYPE BeginFlush() override;
    HRESULT STDMETHODCALLTYPE EndFlush() override;
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *pmt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **pmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *count, int *size) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE **pmt, BYTE *caps) override;

    // IKsPropertySet
    HRESULT STDMETHODCALLTYPE Set(REFGUID propSet, DWORD id, LPVOID instanceData, DWORD instanceLength,
                                  LPVOID propertyData, DWORD dataLength) override;
    HRESULT STDMETHODCALLTYPE Get(REFGUID propSet, DWORD id, LPVOID instanceData, DWORD instanceLength,
                                  LPVOID propertyData, DWORD dataLength, DWORD *returned) override;
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID propSet, DWORD id, DWORD *support) override;

    // IAMLatency / IAMPushSource
    HRESULT STDMETHODCALLTYPE GetLatency(REFERENCE_TIME *latency) override;
    HRESULT STDMETHODCALLTYPE GetPushSourceFlags(ULONG *flags) override;
    HRESULT STDMETHODCALLTYPE SetPushSourceFlags(ULONG flags) override;
    HRESULT STDMETHODCALLTYPE SetStreamOffset(REFERENCE_TIME offset) override;
    HRESULT STDMETHODCALLTYPE GetStreamOffset(REFERENCE_TIME *offset) override;
    HRESULT STDMETHODCALLTYPE GetMaxStreamOffset(REFERENCE_TIME *offset) override;
    HRESULT STDMETHODCALLTYPE SetMaxStreamOffset(REFERENCE_TIME offset) override;

private:
    HRESULT tryConnect(IPin *receivePin, const AM_MEDIA_TYPE &mt);
    void releaseConnection();
    HRESULT decideAllocator(IMemInputPin *input, long bufferSize, IMemAllocator **allocator);
    void streamLoop();
    HRESULT deliver(const uint8_t *nv12, REFERENCE_TIME frameIndex, bool discontinuity);

    VCamFilter *m_filter;
    const uint32_t m_width;
    const uint32_t m_height;
    IPin *m_connected = nullptr;
    IMemInputPin *m_input = nullptr;
    IMemAllocator *m_allocator = nullptr;

    // The streaming thread may change the type (dynamic format change), so
    // it is guarded by its own lock: the thread never takes the filter lock,
    // which Stop() holds while joining it.
    std::mutex m_mtLock;
    AM_MEDIA_TYPE m_mt = {};
    PixelFormat m_format = PixelFormat::YUY2;
    int m_stride = 0;
    bool m_bottomUp = false;
    int m_preferred = -1;  // index set by IAMStreamConfig::SetFormat
    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
};

class VCamFilter final: public IBaseFilter, public IAMFilterMiscFlags {
public:
    VCamFilter();
    ~VCamFilter();

    std::recursive_mutex &lock() { return m_lock; }
    FILTER_STATE state() const { return m_state.load(); }
    REFERENCE_TIME streamTime(REFERENCE_TIME fallback);
    IFilterGraph *graph() const { return m_graph; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IPersist
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID *clsid) override;

    // IMediaFilter
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Pause() override;
    HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME start) override;
    HRESULT STDMETHODCALLTYPE GetState(DWORD timeoutMs, FILTER_STATE *state) override;
    HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock *clock) override;
    HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock **clock) override;

    // IBaseFilter
    HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins **ppEnum) override;
    HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR id, IPin **pin) override;
    HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO *info) override;
    HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph *graph, LPCWSTR name) override;
    HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR *info) override;

    // IAMFilterMiscFlags
    ULONG STDMETHODCALLTYPE GetMiscFlags() override;

private:
    std::atomic<ULONG> m_refCount {1};
    std::recursive_mutex m_lock;
    std::atomic<FILTER_STATE> m_state {State_Stopped};
    REFERENCE_TIME m_start = 0;
    IReferenceClock *m_clock = nullptr;
    IFilterGraph *m_graph = nullptr;  // not AddRef'd: the graph owns us
    wchar_t m_name[MAX_FILTER_NAME] = {};
    OutputSize m_outputSize;  // before m_pin: the pin is built from it
    VCamPin m_pin;
};

}  // namespace wcs
