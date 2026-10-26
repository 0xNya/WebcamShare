// vcamprobe: minimal DirectShow capture client for checking a (virtual) camera.
//   vcamprobe                         list video input devices
//   vcamprobe <name part> [sec] [fmt] capture for sec seconds (default 5),
//                                     fmt = YUY2 | RGB24 | NV12 | any (default any)
// Prints the negotiated format, frame count, fps and simple content stats.
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

// qedit.h is gone from the SDK; the Sample Grabber still ships in qedit.dll.
static const CLSID CLSID_SampleGrabber_ = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const CLSID CLSID_NullRenderer_ = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample *sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE *buffer, long len) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *size, long *buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *callback, long whichMethod) = 0;
};

static void freeMediaType(AM_MEDIA_TYPE *mt)
{
    if (mt->cbFormat)
        CoTaskMemFree(mt->pbFormat);

    if (mt->pUnk)
        mt->pUnk->Release();

    CoTaskMemFree(mt);
}

static std::string fourccName(const GUID &subtype)
{
    if (subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    if (subtype == MEDIASUBTYPE_RGB32) return "RGB32";
    char s[5] = {};
    memcpy(s, &subtype.Data1, 4);

    return s;
}

struct Grabber : ISampleGrabberCB {
    std::atomic<long> frames {0};
    std::atomic<long> changed {0};
    std::atomic<long> bytes {0};
    std::atomic<unsigned> lastSum {0};
    std::atomic<unsigned> lastMean {0};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == __uuidof(ISampleGrabberCB) || riid == IID_IUnknown) {
            *ppv = static_cast<ISampleGrabberCB *>(this);

            return S_OK;
        }

        *ppv = nullptr;

        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample *) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE *buffer, long len) override
    {
        // Sparse checksum: enough to tell a live picture from a frozen one.
        unsigned sum = 0;
        unsigned long long total = 0;
        long step = len / 4096 + 1;
        long n = 0;

        for (long i = 0; i < len; i += step, n++) {
            sum = sum * 31 + buffer[i];
            total += buffer[i];
        }

        if (frames > 0 && sum != lastSum)
            changed++;

        lastSum = sum;
        lastMean = n? unsigned(total / n): 0;
        bytes = len;
        frames++;

        return S_OK;
    }
};

static HRESULT findDevice(const wchar_t *namePart, IBaseFilter **filter, std::wstring &name)
{
    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));

    if (FAILED(hr))
        return hr;

    ComPtr<IEnumMoniker> monikers;

    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK)
        return E_FAIL;

    for (ComPtr<IMoniker> moniker; monikers->Next(1, &moniker, nullptr) == S_OK; moniker.Reset()) {
        ComPtr<IPropertyBag> bag;

        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
            continue;

        VARIANT v;
        VariantInit(&v);

        if (FAILED(bag->Read(L"FriendlyName", &v, nullptr)))
            continue;

        std::wstring friendly = v.bstrVal;
        VariantClear(&v);

        if (!namePart) {
            wprintf(L"  %s\n", friendly.c_str());

            continue;
        }

        if (friendly.find(namePart) != std::wstring::npos) {
            name = friendly;
            wprintf(L"binding: %s\n", friendly.c_str());

            return moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(filter));
        }
    }

    return namePart? HRESULT_FROM_WIN32(ERROR_NOT_FOUND): S_OK;
}

// Selects a 1280x720 format with the wanted subtype on the capture pin.
static HRESULT selectFormat(ICaptureGraphBuilder2 *builder, IBaseFilter *source, const char *fmt)
{
    ComPtr<IAMStreamConfig> config;
    HRESULT hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source,
                                        IID_PPV_ARGS(&config));

    if (FAILED(hr))
        return hr;

    int count = 0;
    int size = 0;
    config->GetNumberOfCapabilities(&count, &size);
    VIDEO_STREAM_CONFIG_CAPS caps;

    for (int i = 0; i < count; i++) {
        AM_MEDIA_TYPE *mt = nullptr;

        if (FAILED(config->GetStreamCaps(i, &mt, reinterpret_cast<BYTE *>(&caps))))
            continue;

        bool match = false;

        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            auto vih = reinterpret_cast<VIDEOINFOHEADER *>(mt->pbFormat);
            auto name = fourccName(mt->subtype);
            printf("  cap %d: %s %ldx%ld @%.1f\n", i, name.c_str(),
                   vih->bmiHeader.biWidth, labs(vih->bmiHeader.biHeight),
                   vih->AvgTimePerFrame? 1e7 / vih->AvgTimePerFrame: 0.0);
            match = vih->bmiHeader.biWidth == 1280
                    && labs(vih->bmiHeader.biHeight) == 720
                    && (_stricmp(fmt, "any") == 0 || _stricmp(fmt, name.c_str()) == 0);
        }

        if (match) {
            hr = config->SetFormat(mt);
            freeMediaType(mt);

            return hr;
        }

        freeMediaType(mt);
    }

    return S_FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    // Unbuffered, so the log survives a crash inside a capture filter.
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (argc < 2) {
        printf("Video input devices:\n");
        ComPtr<IBaseFilter> none;
        std::wstring name;
        findDevice(nullptr, &none, name);

        return 0;
    }

    // vcamprobe <name part> controls: print camera control ranges (zoom etc.).
    if (argc > 2 && _wcsicmp(argv[2], L"controls") == 0) {
        ComPtr<IBaseFilter> device;
        std::wstring name;

        if (FAILED(findDevice(argv[1], &device, name)))
            return 1;

        ComPtr<IAMCameraControl> camera;
        const wchar_t *cameraNames[] = {L"Pan", L"Tilt", L"Roll", L"Zoom", L"Exposure", L"Iris", L"Focus"};

        if (SUCCEEDED(device.As(&camera))) {
            for (long p = CameraControl_Pan; p <= CameraControl_Focus; p++) {
                long min, max, step, def, caps, value, flags;

                if (SUCCEEDED(camera->GetRange(p, &min, &max, &step, &def, &caps))
                    && SUCCEEDED(camera->Get(p, &value, &flags)))
                    wprintf(L"  %-8s value=%ld range=%ld..%ld step=%ld default=%ld %s\n", cameraNames[p], value,
                            min, max, step, def, flags & CameraControl_Flags_Auto? L"auto": L"");
            }
        } else {
            printf("  no IAMCameraControl\n");
        }

        return 0;
    }

    int seconds = argc > 2? _wtoi(argv[2]): 5;
    char fmt[16] = "any";

    if (argc > 3)
        WideCharToMultiByte(CP_ACP, 0, argv[3], -1, fmt, sizeof(fmt), nullptr, nullptr);

    ComPtr<IBaseFilter> source;
    std::wstring name;
    HRESULT hr = findDevice(argv[1], &source, name);

    if (FAILED(hr)) {
        printf("device not found / bind failed: 0x%08lx\n", hr);

        return 1;
    }

    wprintf(L"device: %s\n", name.c_str());
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    builder->SetFiltergraph(graph.Get());
    graph->AddFilter(source.Get(), L"source");

    hr = selectFormat(builder.Get(), source.Get(), fmt);
    printf("set format %s: 0x%08lx\n", fmt, hr);

    ComPtr<IBaseFilter> grabberFilter;
    ComPtr<IBaseFilter> nullRenderer;
    hr = CoCreateInstance(CLSID_SampleGrabber_, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter));

    if (FAILED(hr)) {
        printf("no sample grabber: 0x%08lx\n", hr);

        return 1;
    }

    CoCreateInstance(CLSID_NullRenderer_, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer));
    graph->AddFilter(grabberFilter.Get(), L"grabber");
    graph->AddFilter(nullRenderer.Get(), L"null");

    ComPtr<ISampleGrabber> grabber;
    grabberFilter.As(&grabber);
    AM_MEDIA_TYPE want = {};
    want.majortype = MEDIATYPE_Video;
    grabber->SetMediaType(&want);
    Grabber cb;
    grabber->SetCallback(&cb, 1);

    hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source.Get(),
                               grabberFilter.Get(), nullRenderer.Get());
    printf("render: 0x%08lx\n", hr);

    if (FAILED(hr))
        return 1;

    AM_MEDIA_TYPE connected = {};

    if (SUCCEEDED(grabber->GetConnectedMediaType(&connected))
        && connected.formattype == FORMAT_VideoInfo) {
        auto vih = reinterpret_cast<VIDEOINFOHEADER *>(connected.pbFormat);
        printf("connected: %s %ldx%ld\n", fourccName(connected.subtype).c_str(),
               vih->bmiHeader.biWidth, labs(vih->bmiHeader.biHeight));
        CoTaskMemFree(connected.pbFormat);
    }

    ComPtr<IMediaControl> control;
    graph.As(&control);
    hr = control->Run();
    printf("run: 0x%08lx\n", hr);
    auto t0 = GetTickCount64();

    for (int s = 0; s < seconds; s++) {
        Sleep(1000);
        printf("  t=%ds frames=%ld changed=%ld bytes=%ld mean=%u\n", s + 1,
               cb.frames.load(), cb.changed.load(), cb.bytes.load(), cb.lastMean.load());
    }

    double elapsed = (GetTickCount64() - t0) / 1000.0;
    control->Stop();
    printf("RESULT frames=%ld fps=%.1f changed=%ld\n", cb.frames.load(),
           cb.frames / elapsed, cb.changed.load());
    CoUninitialize();

    return cb.frames > 0? 0: 2;
}
