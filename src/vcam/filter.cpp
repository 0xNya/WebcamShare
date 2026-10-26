#include "filter.h"

#include <dvdmedia.h>
#include <ks.h>
#include <ksmedia.h>
#include <uuids.h>
#include <vfwmsgs.h>

#include <cstring>
#include <vector>

#include "../common/shared_frame.h"
#include "../common/strings.h"

namespace wcs {

namespace {

struct FormatInfo {
    PixelFormat format;
    const GUID *subtype;
    DWORD compression;
};

const FormatInfo kFormats[] = {
    {PixelFormat::YUY2, &MEDIASUBTYPE_YUY2, MAKEFOURCC('Y', 'U', 'Y', '2')},
    {PixelFormat::NV12, &MEDIASUBTYPE_NV12, MAKEFOURCC('N', 'V', '1', '2')},
    {PixelFormat::RGB24, &MEDIASUBTYPE_RGB24, BI_RGB},
    {PixelFormat::RGB32, &MEDIASUBTYPE_RGB32, BI_RGB},
};
constexpr int kFormatCount = int(sizeof(kFormats) / sizeof(kFormats[0]));

int frameBytes(PixelFormat format, int stride, uint32_t height)
{
    return format == PixelFormat::NV12? stride * int(height) * 3 / 2: stride * int(height);
}

HRESULT buildMediaType(int index, uint32_t width, uint32_t height, AM_MEDIA_TYPE *mt)
{
    if (index < 0 || index >= kFormatCount)
        return VFW_S_NO_MORE_ITEMS;

    auto &info = kFormats[index];
    auto vih = static_cast<VIDEOINFOHEADER *>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));

    if (!vih)
        return E_OUTOFMEMORY;

    int bpp = bitsPerPixel(info.format);
    int size = int(width * height) * bpp / 8;
    memset(vih, 0, sizeof(VIDEOINFOHEADER));
    vih->AvgTimePerFrame = kFrameDuration;
    vih->dwBitRate = DWORD(size) * 8 * kFps;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = LONG(width);
    vih->bmiHeader.biHeight = LONG(height);  // RGB: bottom-up; YUV: always top-down
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = WORD(bpp);
    vih->bmiHeader.biCompression = info.compression;
    vih->bmiHeader.biSizeImage = DWORD(size);

    memset(mt, 0, sizeof(AM_MEDIA_TYPE));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *info.subtype;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = ULONG(size);
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = reinterpret_cast<BYTE *>(vih);

    return S_OK;
}

// Accepts one of our formats at width x height; a wider biWidth means a
// padded stride (renderers ask for that). Returns the format index or -1.
int parseMediaType(const AM_MEDIA_TYPE *mt, uint32_t width, uint32_t height, int *stride, bool *bottomUp)
{
    if (!mt || mt->majortype != MEDIATYPE_Video || !mt->pbFormat)
        return -1;

    const BITMAPINFOHEADER *bih = nullptr;

    if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER))
        bih = &reinterpret_cast<const VIDEOINFOHEADER *>(mt->pbFormat)->bmiHeader;
    else if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2))
        bih = &reinterpret_cast<const VIDEOINFOHEADER2 *>(mt->pbFormat)->bmiHeader;

    if (!bih || bih->biWidth < LONG(width) || labs(bih->biHeight) != LONG(height))
        return -1;

    for (int i = 0; i < kFormatCount; i++) {
        if (mt->subtype != *kFormats[i].subtype)
            continue;

        int bpp = bitsPerPixel(kFormats[i].format);

        if (stride) {
            *stride = kFormats[i].format == PixelFormat::NV12?
                          bih->biWidth: (bih->biWidth * bpp / 8 + 3) & ~3;
        }

        if (bottomUp) {
            bool rgb = kFormats[i].compression == BI_RGB;
            *bottomUp = rgb && bih->biHeight > 0;
        }

        return i;
    }

    return -1;
}

// Partially specified types (GUID_NULL fields) act as wildcards.
bool matchesTemplate(const AM_MEDIA_TYPE &mt, const AM_MEDIA_TYPE *pattern)
{
    if (!pattern)
        return true;

    if (pattern->majortype != GUID_NULL && pattern->majortype != mt.majortype)
        return false;

    if (pattern->subtype != GUID_NULL && pattern->subtype != mt.subtype)
        return false;

    if (pattern->formattype != GUID_NULL && pattern->formattype != mt.formattype)
        return false;

    return true;
}

class EnumPinsImpl final: public IEnumPins {
public:
    EnumPinsImpl(VCamFilter *filter, IPin *pin, ULONG position):
        m_filter(filter), m_pin(pin), m_position(position)
    {
        m_filter->AddRef();
        g_objectCount++;
    }

    ~EnumPinsImpl()
    {
        m_filter->Release();
        g_objectCount--;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins *>(this);
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

    HRESULT STDMETHODCALLTYPE Next(ULONG count, IPin **pins, ULONG *fetched) override
    {
        if (!pins || (count > 1 && !fetched))
            return E_POINTER;

        ULONG n = 0;

        if (count > 0 && m_position == 0) {
            pins[0] = m_pin;
            m_pin->AddRef();
            m_position = 1;
            n = 1;
        }

        if (fetched)
            *fetched = n;

        return n == count? S_OK: S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    {
        m_position += count;

        return m_position <= 1? S_OK: S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        m_position = 0;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumPins **ppEnum) override
    {
        if (!ppEnum)
            return E_POINTER;

        *ppEnum = new (std::nothrow) EnumPinsImpl(m_filter, m_pin, m_position);

        return *ppEnum? S_OK: E_OUTOFMEMORY;
    }

private:
    std::atomic<ULONG> m_refCount {1};
    VCamFilter *m_filter;
    IPin *m_pin;
    ULONG m_position;
};

class EnumMediaTypesImpl final: public IEnumMediaTypes {
public:
    EnumMediaTypesImpl(VCamFilter *filter, VCamPin *pin, ULONG position):
        m_filter(filter), m_pin(pin), m_position(position)
    {
        m_filter->AddRef();
        g_objectCount++;
    }

    ~EnumMediaTypesImpl()
    {
        m_filter->Release();
        g_objectCount--;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes *>(this);
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

    HRESULT STDMETHODCALLTYPE Next(ULONG count, AM_MEDIA_TYPE **types, ULONG *fetched) override
    {
        if (!types || (count > 1 && !fetched))
            return E_POINTER;

        ULONG n = 0;

        while (n < count) {
            auto mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));

            if (!mt)
                break;

            if (m_pin->mediaType(int(m_position), mt) != S_OK) {
                CoTaskMemFree(mt);

                break;
            }

            types[n++] = mt;
            m_position++;
        }

        if (fetched)
            *fetched = n;

        return n == count? S_OK: S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    {
        m_position += count;
        AM_MEDIA_TYPE mt;

        if (m_pin->mediaType(int(m_position) - 1, &mt) != S_OK)
            return S_FALSE;

        freeMediaType(mt);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        m_position = 0;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes **ppEnum) override
    {
        if (!ppEnum)
            return E_POINTER;

        *ppEnum = new (std::nothrow) EnumMediaTypesImpl(m_filter, m_pin, m_position);

        return *ppEnum? S_OK: E_OUTOFMEMORY;
    }

private:
    std::atomic<ULONG> m_refCount {1};
    VCamFilter *m_filter;
    VCamPin *m_pin;
    ULONG m_position;
};

}  // namespace

void freeMediaType(AM_MEDIA_TYPE &mt)
{
    if (mt.cbFormat && mt.pbFormat)
        CoTaskMemFree(mt.pbFormat);

    if (mt.pUnk)
        mt.pUnk->Release();

    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    mt.pUnk = nullptr;
}

void deleteMediaType(AM_MEDIA_TYPE *mt)
{
    if (mt) {
        freeMediaType(*mt);
        CoTaskMemFree(mt);
    }
}

HRESULT copyMediaType(AM_MEDIA_TYPE &dst, const AM_MEDIA_TYPE &src)
{
    dst = src;

    if (src.cbFormat && src.pbFormat) {
        dst.pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(src.cbFormat));

        if (!dst.pbFormat) {
            dst.cbFormat = 0;

            return E_OUTOFMEMORY;
        }

        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    } else {
        dst.pbFormat = nullptr;
        dst.cbFormat = 0;
    }

    if (dst.pUnk)
        dst.pUnk->AddRef();

    return S_OK;
}

// --- VCamPin --------------------------------------------------------------

VCamPin::VCamPin(VCamFilter *filter, uint32_t width, uint32_t height):
    m_filter(filter),
    m_width(width),
    m_height(height)
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

VCamPin::~VCamPin()
{
    inactive();
    releaseConnection();

    if (m_stopEvent)
        CloseHandle(m_stopEvent);
}

HRESULT VCamPin::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IPin)
        *ppv = static_cast<IPin *>(this);
    else if (riid == IID_IAMStreamConfig)
        *ppv = static_cast<IAMStreamConfig *>(this);
    else if (riid == IID_IKsPropertySet)
        *ppv = static_cast<IKsPropertySet *>(this);
    else if (riid == IID_IAMPushSource)
        *ppv = static_cast<IAMPushSource *>(this);
    else if (riid == IID_IAMLatency)
        *ppv = static_cast<IAMLatency *>(this);
    else {
        *ppv = nullptr;

        return E_NOINTERFACE;
    }

    AddRef();

    return S_OK;
}

ULONG VCamPin::AddRef()
{
    return m_filter->AddRef();
}

ULONG VCamPin::Release()
{
    return m_filter->Release();
}

HRESULT VCamPin::mediaType(int index, AM_MEDIA_TYPE *mt)
{
    if (!mt)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());

    // After SetFormat() only that format is offered.
    if (m_preferred >= 0)
        return index == 0? buildMediaType(m_preferred, m_width, m_height, mt): VFW_S_NO_MORE_ITEMS;

    return buildMediaType(index, m_width, m_height, mt);
}

HRESULT VCamPin::Connect(IPin *receivePin, const AM_MEDIA_TYPE *pmt)
{
    if (!receivePin)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());

    if (m_connected)
        return VFW_E_ALREADY_CONNECTED;

    if (m_filter->state() != State_Stopped)
        return VFW_E_NOT_STOPPED;

    HRESULT result = VFW_E_NO_ACCEPTABLE_TYPES;

    for (int i = 0;; i++) {
        AM_MEDIA_TYPE mt;

        if (mediaType(i, &mt) != S_OK)
            break;

        if (matchesTemplate(mt, pmt)) {
            // A fully specified type from the caller wins over ours (stride).
            bool exact = pmt && pmt->pbFormat && parseMediaType(pmt, m_width, m_height, nullptr, nullptr);
            result = tryConnect(receivePin, exact? *pmt: mt);
        }

        freeMediaType(mt);

        if (SUCCEEDED(result))
            return result;
    }

    return result;
}

HRESULT VCamPin::tryConnect(IPin *receivePin, const AM_MEDIA_TYPE &mt)
{
    int stride = 0;
    bool bottomUp = false;
    int index = parseMediaType(&mt, m_width, m_height, &stride, &bottomUp);

    if (index < 0)
        return VFW_E_TYPE_NOT_ACCEPTED;

    HRESULT hr = receivePin->ReceiveConnection(this, &mt);

    if (FAILED(hr))
        return hr;

    IMemInputPin *input = nullptr;
    hr = receivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void **>(&input));

    if (FAILED(hr)) {
        receivePin->Disconnect();

        return hr;
    }

    IMemAllocator *allocator = nullptr;
    hr = decideAllocator(input, frameBytes(kFormats[index].format, stride, m_height), &allocator);

    if (FAILED(hr)) {
        input->Release();
        receivePin->Disconnect();

        return hr;
    }

    hr = copyMediaType(m_mt, mt);

    if (FAILED(hr)) {
        allocator->Release();
        input->Release();
        receivePin->Disconnect();

        return hr;
    }

    m_connected = receivePin;
    m_connected->AddRef();
    m_input = input;
    m_allocator = allocator;
    m_format = kFormats[index].format;
    m_stride = stride;
    m_bottomUp = bottomUp;

    return S_OK;
}

HRESULT VCamPin::decideAllocator(IMemInputPin *input, long bufferSize, IMemAllocator **allocator)
{
    ALLOCATOR_PROPERTIES request = {};

    if (FAILED(input->GetAllocatorRequirements(&request)))
        request = {};

    request.cBuffers = request.cBuffers > 4? request.cBuffers: 4;
    request.cbBuffer = request.cbBuffer > bufferSize? request.cbBuffer: bufferSize;
    request.cbAlign = request.cbAlign > 0? request.cbAlign: 1;

    IMemAllocator *candidate = nullptr;

    // Prefer the downstream allocator, fall back to a standard one.
    for (int attempt = 0; attempt < 2; attempt++) {
        HRESULT hr = attempt == 0?
                         input->GetAllocator(&candidate):
                         CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IMemAllocator, reinterpret_cast<void **>(&candidate));

        if (FAILED(hr) || !candidate)
            continue;

        ALLOCATOR_PROPERTIES actual = {};

        if (SUCCEEDED(candidate->SetProperties(&request, &actual))
            && actual.cbBuffer >= bufferSize
            && SUCCEEDED(input->NotifyAllocator(candidate, FALSE))) {
            *allocator = candidate;

            return S_OK;
        }

        candidate->Release();
        candidate = nullptr;
    }

    return VFW_E_NO_ALLOCATOR;
}

HRESULT VCamPin::ReceiveConnection(IPin *, const AM_MEDIA_TYPE *)
{
    // Output pins do not accept connections.
    return E_UNEXPECTED;
}

HRESULT VCamPin::Disconnect()
{
    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());

    if (m_filter->state() != State_Stopped)
        return VFW_E_NOT_STOPPED;

    if (!m_connected)
        return S_FALSE;

    releaseConnection();

    return S_OK;
}

void VCamPin::releaseConnection()
{
    if (m_allocator) {
        m_allocator->Decommit();
        m_allocator->Release();
        m_allocator = nullptr;
    }

    if (m_input) {
        m_input->Release();
        m_input = nullptr;
    }

    if (m_connected) {
        m_connected->Release();
        m_connected = nullptr;
    }

    std::lock_guard<std::mutex> mtLock(m_mtLock);
    freeMediaType(m_mt);
    memset(&m_mt, 0, sizeof(m_mt));
}

HRESULT VCamPin::ConnectedTo(IPin **pin)
{
    if (!pin)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());
    *pin = m_connected;

    if (!m_connected)
        return VFW_E_NOT_CONNECTED;

    m_connected->AddRef();

    return S_OK;
}

HRESULT VCamPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());

    if (!m_connected) {
        memset(pmt, 0, sizeof(AM_MEDIA_TYPE));

        return VFW_E_NOT_CONNECTED;
    }

    std::lock_guard<std::mutex> mtLock(m_mtLock);

    return copyMediaType(*pmt, m_mt);
}

HRESULT VCamPin::QueryPinInfo(PIN_INFO *info)
{
    if (!info)
        return E_POINTER;

    info->pFilter = m_filter;
    m_filter->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcsncpy_s(info->achName, kPinName, _TRUNCATE);

    return S_OK;
}

HRESULT VCamPin::QueryDirection(PIN_DIRECTION *dir)
{
    if (!dir)
        return E_POINTER;

    *dir = PINDIR_OUTPUT;

    return S_OK;
}

HRESULT VCamPin::QueryId(LPWSTR *id)
{
    if (!id)
        return E_POINTER;

    size_t bytes = sizeof(kPinName);
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));

    if (!*id)
        return E_OUTOFMEMORY;

    memcpy(*id, kPinName, bytes);

    return S_OK;
}

HRESULT VCamPin::QueryAccept(const AM_MEDIA_TYPE *pmt)
{
    return parseMediaType(pmt, m_width, m_height, nullptr, nullptr) >= 0? S_OK: S_FALSE;
}

HRESULT VCamPin::EnumMediaTypes(IEnumMediaTypes **ppEnum)
{
    if (!ppEnum)
        return E_POINTER;

    *ppEnum = new (std::nothrow) EnumMediaTypesImpl(m_filter, this, 0);

    return *ppEnum? S_OK: E_OUTOFMEMORY;
}

HRESULT VCamPin::QueryInternalConnections(IPin **, ULONG *)
{
    return E_NOTIMPL;
}

HRESULT VCamPin::EndOfStream()
{
    return E_UNEXPECTED;
}

HRESULT VCamPin::BeginFlush()
{
    return E_UNEXPECTED;
}

HRESULT VCamPin::EndFlush()
{
    return E_UNEXPECTED;
}

HRESULT VCamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double)
{
    return S_OK;
}

// IAMStreamConfig

HRESULT VCamPin::SetFormat(AM_MEDIA_TYPE *pmt)
{
    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());

    if (!pmt) {
        m_preferred = -1;

        return S_OK;
    }

    int index = parseMediaType(pmt, m_width, m_height, nullptr, nullptr);

    if (index < 0)
        return VFW_E_INVALIDMEDIATYPE;

    if (m_connected) {
        if (kFormats[index].format == m_format)
            return S_OK;

        if (m_filter->state() != State_Stopped)
            return VFW_E_NOT_STOPPED;

        m_preferred = index;
        auto graph = m_filter->graph();

        return graph? graph->Reconnect(this): VFW_E_NOT_IN_GRAPH;
    }

    m_preferred = index;

    return S_OK;
}

HRESULT VCamPin::GetFormat(AM_MEDIA_TYPE **pmt)
{
    if (!pmt)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_filter->lock());
    auto mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));

    if (!mt)
        return E_OUTOFMEMORY;

    HRESULT hr;

    if (m_connected) {
        std::lock_guard<std::mutex> mtLock(m_mtLock);
        hr = copyMediaType(*mt, m_mt);
    } else {
        hr = mediaType(0, mt);
    }

    if (hr != S_OK) {
        CoTaskMemFree(mt);
        *pmt = nullptr;

        return FAILED(hr)? hr: E_FAIL;
    }

    *pmt = mt;

    return S_OK;
}

HRESULT VCamPin::GetNumberOfCapabilities(int *count, int *size)
{
    if (!count || !size)
        return E_POINTER;

    *count = kFormatCount;
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);

    return S_OK;
}

HRESULT VCamPin::GetStreamCaps(int index, AM_MEDIA_TYPE **pmt, BYTE *capsData)
{
    if (!pmt || !capsData)
        return E_POINTER;

    *pmt = nullptr;

    if (index < 0)
        return E_INVALIDARG;

    if (index >= kFormatCount)
        return S_FALSE;

    auto mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));

    if (!mt)
        return E_OUTOFMEMORY;

    HRESULT hr = buildMediaType(index, m_width, m_height, mt);

    if (hr != S_OK) {
        CoTaskMemFree(mt);

        return FAILED(hr)? hr: S_FALSE;
    }

    auto caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(capsData);
    memset(caps, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    SIZE size = {LONG(m_width), LONG(m_height)};
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize = size;
    caps->MinCroppingSize = size;
    caps->MaxCroppingSize = size;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->MinOutputSize = size;
    caps->MaxOutputSize = size;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = kFrameDuration;
    caps->MaxFrameInterval = kFrameDuration;
    auto vih = reinterpret_cast<VIDEOINFOHEADER *>(mt->pbFormat);
    caps->MinBitsPerSecond = LONG(vih->dwBitRate);
    caps->MaxBitsPerSecond = LONG(vih->dwBitRate);
    *pmt = mt;

    return S_OK;
}

// IKsPropertySet: clients find the capture pin by its category.

HRESULT VCamPin::Set(REFGUID propSet, DWORD, LPVOID, DWORD, LPVOID, DWORD)
{
    return propSet == AMPROPSETID_Pin? E_PROP_ID_UNSUPPORTED: E_PROP_SET_UNSUPPORTED;
}

HRESULT VCamPin::Get(REFGUID propSet, DWORD id, LPVOID, DWORD,
                     LPVOID propertyData, DWORD dataLength, DWORD *returned)
{
    if (propSet != AMPROPSETID_Pin)
        return E_PROP_SET_UNSUPPORTED;

    if (id != AMPROPERTY_PIN_CATEGORY)
        return E_PROP_ID_UNSUPPORTED;

    if (returned)
        *returned = sizeof(GUID);

    if (!propertyData)
        return returned? S_OK: E_POINTER;

    if (dataLength < sizeof(GUID))
        return E_UNEXPECTED;

    *static_cast<GUID *>(propertyData) = PIN_CATEGORY_CAPTURE;

    return S_OK;
}

HRESULT VCamPin::QuerySupported(REFGUID propSet, DWORD id, DWORD *support)
{
    if (propSet != AMPROPSETID_Pin)
        return E_PROP_SET_UNSUPPORTED;

    if (id != AMPROPERTY_PIN_CATEGORY)
        return E_PROP_ID_UNSUPPORTED;

    if (support)
        *support = KSPROPERTY_SUPPORT_GET;

    return S_OK;
}

// IAMLatency / IAMPushSource

HRESULT VCamPin::GetLatency(REFERENCE_TIME *latency)
{
    if (!latency)
        return E_POINTER;

    *latency = kFrameDuration;

    return S_OK;
}

HRESULT VCamPin::GetPushSourceFlags(ULONG *flags)
{
    if (!flags)
        return E_POINTER;

    *flags = 0;

    return S_OK;
}

HRESULT VCamPin::SetPushSourceFlags(ULONG)
{
    return E_NOTIMPL;
}

HRESULT VCamPin::SetStreamOffset(REFERENCE_TIME)
{
    return E_NOTIMPL;
}

HRESULT VCamPin::GetStreamOffset(REFERENCE_TIME *offset)
{
    if (!offset)
        return E_POINTER;

    *offset = 0;

    return S_OK;
}

HRESULT VCamPin::GetMaxStreamOffset(REFERENCE_TIME *offset)
{
    if (!offset)
        return E_POINTER;

    *offset = 0;

    return S_OK;
}

HRESULT VCamPin::SetMaxStreamOffset(REFERENCE_TIME)
{
    return E_NOTIMPL;
}

// Streaming

HRESULT VCamPin::active()
{
    if (!m_connected || m_thread.joinable())
        return S_OK;

    HRESULT hr = m_allocator->Commit();

    if (FAILED(hr))
        return hr;

    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { streamLoop(); });

    return S_OK;
}

void VCamPin::inactive()
{
    if (!m_thread.joinable())
        return;

    SetEvent(m_stopEvent);

    // Unblocks a GetBuffer() waiting for downstream to return samples.
    if (m_allocator)
        m_allocator->Decommit();

    m_thread.join();
}

void VCamPin::streamLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    FrameReader reader;
    std::vector<uint8_t> frame(kMaxFrameSize);
    std::vector<uint8_t> scaled;  // frame resized to our size, when they differ
    std::vector<uint8_t> placeholder;
    FrameInfo info;
    LONG64 scaledSeq = -1;
    bool haveFrame = false;
    bool discontinuity = true;
    REFERENCE_TIME frameIndex = 0;
    DWORD frameMs = DWORD(kFrameDuration / 10000);

    for (;;) {
        bool open = reader.tryOpen();

        // A new frame normally arrives within one frame time; if the app
        // stalls, repeat the last frame so clients keep receiving video.
        if (!reader.waitNewer(info.seq, open && reader.writerAlive()? 2 * frameMs: frameMs, m_stopEvent))
            break;

        const uint8_t *source = nullptr;

        if (open && reader.readLatest(frame.data(), &info)) {
            haveFrame = true;
        } else if (!(open && reader.writerAlive() && haveFrame)) {
            haveFrame = false;  // else: torn copy, resend the previous frame
        }

        if (haveFrame && info.width == m_width && info.height == m_height) {
            source = frame.data();
        } else if (haveFrame) {
            // The output size was changed while this client kept the camera
            // open: scale until it reopens the camera with the new size.
            if (scaledSeq != info.seq) {
                scaled.resize(nv12Size(m_width, m_height));
                scaleNv12(frame.data(), info.width, info.height, scaled.data(), m_width, m_height);
                scaledSeq = info.seq;
            }

            source = scaled.data();
        } else {
            if (placeholder.empty()) {
                placeholder.resize(nv12Size(m_width, m_height));
                setRussian(resolvesToRussian(readLanguage()));
                renderPlaceholder(placeholder.data(), m_width, m_height, L"WebcamShare",
                                  tr(Str::PlaceholderAppNotRunning));
            }

            source = placeholder.data();
        }

        if (m_filter->state() != State_Running) {
            discontinuity = true;

            continue;
        }

        HRESULT hr = deliver(source, frameIndex++, discontinuity);

        if (hr == VFW_E_NOT_COMMITTED)
            break;  // Stop() decommitted the allocator

        discontinuity = FAILED(hr);
    }

    CoUninitialize();
}

HRESULT VCamPin::deliver(const uint8_t *nv12, REFERENCE_TIME frameIndex, bool discontinuity)
{
    IMediaSample *sample = nullptr;
    HRESULT hr = m_allocator->GetBuffer(&sample, nullptr, nullptr, 0);

    if (FAILED(hr))
        return hr;

    // Downstream may ask for a new format (e.g. a padded stride).
    AM_MEDIA_TYPE *changed = nullptr;

    if (sample->GetMediaType(&changed) == S_OK && changed) {
        int stride = 0;
        bool bottomUp = false;
        int index = parseMediaType(changed, m_width, m_height, &stride, &bottomUp);

        if (index >= 0) {
            std::lock_guard<std::mutex> mtLock(m_mtLock);
            freeMediaType(m_mt);
            copyMediaType(m_mt, *changed);
            m_format = kFormats[index].format;
            m_stride = stride;
            m_bottomUp = bottomUp;
        }

        deleteMediaType(changed);
    }

    BYTE *buffer = nullptr;
    int needed = frameBytes(m_format, m_stride, m_height);

    if (FAILED(sample->GetPointer(&buffer)) || sample->GetSize() < needed) {
        sample->Release();

        return E_FAIL;
    }

    convertFromNv12(nv12, m_width, m_height, m_format, buffer, m_stride, m_bottomUp);
    sample->SetActualDataLength(needed);

    REFERENCE_TIME start = m_filter->streamTime(frameIndex * kFrameDuration);
    REFERENCE_TIME stop = start + kFrameDuration;
    sample->SetTime(&start, &stop);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(discontinuity? TRUE: FALSE);
    hr = m_input->Receive(sample);
    sample->Release();

    return hr;
}

// --- VCamFilter -------------------------------------------------------------

VCamFilter::VCamFilter():
    m_outputSize(readOutputSize()),
    m_pin(this, m_outputSize.width, m_outputSize.height)
{
    g_objectCount++;
}

VCamFilter::~VCamFilter()
{
    m_pin.inactive();

    if (m_clock)
        m_clock->Release();

    g_objectCount--;
}

REFERENCE_TIME VCamFilter::streamTime(REFERENCE_TIME fallback)
{
    // Called from the streaming thread: the clock is only swapped while
    // stopped, so no lock is needed here (and taking the filter lock could
    // deadlock against Stop()).
    REFERENCE_TIME now = 0;

    if (m_clock && SUCCEEDED(m_clock->GetTime(&now)))
        return now - m_start;

    return fallback;
}

HRESULT VCamFilter::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IBaseFilter)
        *ppv = static_cast<IBaseFilter *>(this);
    else if (riid == IID_IMediaFilter)
        *ppv = static_cast<IMediaFilter *>(this);
    else if (riid == IID_IPersist)
        *ppv = static_cast<IPersist *>(this);
    else if (riid == IID_IAMFilterMiscFlags)
        *ppv = static_cast<IAMFilterMiscFlags *>(this);
    else {
        *ppv = nullptr;

        return E_NOINTERFACE;
    }

    AddRef();

    return S_OK;
}

ULONG VCamFilter::AddRef()
{
    return ++m_refCount;
}

ULONG VCamFilter::Release()
{
    ULONG ref = --m_refCount;

    if (!ref)
        delete this;

    return ref;
}

HRESULT VCamFilter::GetClassID(CLSID *clsid)
{
    if (!clsid)
        return E_POINTER;

    *clsid = CLSID_WebcamShareCam;

    return S_OK;
}

HRESULT VCamFilter::Stop()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_pin.inactive();
    m_state = State_Stopped;

    return S_OK;
}

HRESULT VCamFilter::Pause()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (m_state == State_Stopped) {
        HRESULT hr = m_pin.active();

        if (FAILED(hr))
            return hr;
    }

    m_state = State_Paused;

    // Live source: no data is produced while paused.
    return VFW_S_CANT_CUE;
}

HRESULT VCamFilter::Run(REFERENCE_TIME start)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (m_state == State_Stopped) {
        HRESULT hr = Pause();

        if (FAILED(hr))
            return hr;
    }

    m_start = start;
    m_state = State_Running;

    return S_OK;
}

HRESULT VCamFilter::GetState(DWORD, FILTER_STATE *state)
{
    if (!state)
        return E_POINTER;

    *state = m_state;

    return m_state == State_Paused? VFW_S_CANT_CUE: S_OK;
}

HRESULT VCamFilter::SetSyncSource(IReferenceClock *clock)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (clock)
        clock->AddRef();

    if (m_clock)
        m_clock->Release();

    m_clock = clock;

    return S_OK;
}

HRESULT VCamFilter::GetSyncSource(IReferenceClock **clock)
{
    if (!clock)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    *clock = m_clock;

    if (m_clock)
        m_clock->AddRef();

    return S_OK;
}

HRESULT VCamFilter::EnumPins(IEnumPins **ppEnum)
{
    if (!ppEnum)
        return E_POINTER;

    *ppEnum = new (std::nothrow) EnumPinsImpl(this, &m_pin, 0);

    return *ppEnum? S_OK: E_OUTOFMEMORY;
}

HRESULT VCamFilter::FindPin(LPCWSTR id, IPin **pin)
{
    if (!id || !pin)
        return E_POINTER;

    if (wcscmp(id, kPinName) != 0) {
        *pin = nullptr;

        return VFW_E_NOT_FOUND;
    }

    *pin = &m_pin;
    m_pin.AddRef();

    return S_OK;
}

HRESULT VCamFilter::QueryFilterInfo(FILTER_INFO *info)
{
    if (!info)
        return E_POINTER;

    std::lock_guard<std::recursive_mutex> lock(m_lock);
    wcsncpy_s(info->achName, m_name, _TRUNCATE);
    info->pGraph = m_graph;

    if (m_graph)
        m_graph->AddRef();

    return S_OK;
}

HRESULT VCamFilter::JoinFilterGraph(IFilterGraph *graph, LPCWSTR name)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_graph = graph;

    if (name)
        wcsncpy_s(m_name, name, _TRUNCATE);
    else
        m_name[0] = 0;

    return S_OK;
}

HRESULT VCamFilter::QueryVendorInfo(LPWSTR *)
{
    return E_NOTIMPL;
}

ULONG VCamFilter::GetMiscFlags()
{
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

}  // namespace wcs
