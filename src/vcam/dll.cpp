// COM server entry points and registration of the virtual camera as a
// DirectShow video input device ("regsvr32 WebcamShareCam.dll", admin).
#include "filter.h"

#include <uuids.h>

#include <new>
#include <string>

namespace wcs {

// {AAD9A7D0-FC31-43B2-A807-FB7BA56DF5D3}
const CLSID CLSID_WebcamShareCam =
    {0xaad9a7d0, 0xfc31, 0x43b2, {0xa8, 0x07, 0xfb, 0x7b, 0xa5, 0x6d, 0xf5, 0xd3}};

std::atomic<long> g_objectCount {0};

}  // namespace wcs

using namespace wcs;

static HMODULE g_module = nullptr;
static std::atomic<long> g_lockCount {0};

namespace {

class ClassFactory final: public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);

            return S_OK;
        }

        *ppv = nullptr;

        return E_NOINTERFACE;
    }

    // Static object: lifetime is the module's.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (outer)
            return CLASS_E_NOAGGREGATION;

        auto filter = new (std::nothrow) VCamFilter;

        if (!filter)
            return E_OUTOFMEMORY;

        HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();

        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock)
            g_lockCount++;
        else
            g_lockCount--;

        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring clsidString()
{
    wchar_t buffer[64] = {};
    StringFromGUID2(CLSID_WebcamShareCam, buffer, 64);

    return buffer;
}

HRESULT setRegistryString(HKEY root, const std::wstring &path, const wchar_t *name,
                          const std::wstring &value)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                                     &key, nullptr);

    if (status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(status);

    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                            DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    return HRESULT_FROM_WIN32(status);
}

HRESULT withFilterMapper(HRESULT (*action)(IFilterMapper2 *))
{
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFilterMapper2 *mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void **>(&mapper));

    if (SUCCEEDED(hr)) {
        hr = action(mapper);
        mapper->Release();
    }

    if (SUCCEEDED(init))
        CoUninitialize();

    return hr;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv)
        return E_POINTER;

    *ppv = nullptr;

    if (clsid != CLSID_WebcamShareCam)
        return CLASS_E_CLASSNOTAVAILABLE;

    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return g_objectCount == 0 && g_lockCount == 0? S_OK: S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    auto key = L"CLSID\\" + clsidString();
    HRESULT hr = setRegistryString(HKEY_CLASSES_ROOT, key, nullptr, kFilterName);

    if (SUCCEEDED(hr))
        hr = setRegistryString(HKEY_CLASSES_ROOT, key + L"\\InprocServer32", nullptr, path);

    if (SUCCEEDED(hr))
        hr = setRegistryString(HKEY_CLASSES_ROOT, key + L"\\InprocServer32", L"ThreadingModel",
                               L"Both");

    if (FAILED(hr))
        return hr;

    return withFilterMapper([](IFilterMapper2 *mapper) -> HRESULT {
        static const REGPINTYPES pinType = {&MEDIATYPE_Video, &MEDIASUBTYPE_NULL};
        REGFILTERPINS2 pin = {};
        pin.dwFlags = REG_PINFLAG_B_OUTPUT;
        pin.cInstances = 1;
        pin.nMediaTypes = 1;
        pin.lpMediaType = &pinType;
        pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
        REGFILTER2 filter = {};
        filter.dwVersion = 2;
        filter.dwMerit = MERIT_DO_NOT_USE;
        filter.cPins2 = 1;
        filter.rgPins2 = &pin;
        auto instance = clsidString();

        return mapper->RegisterFilter(CLSID_WebcamShareCam, kFilterName, nullptr,
                                      &CLSID_VideoInputDeviceCategory, instance.c_str(),
                                      &filter);
    });
}

STDAPI DllUnregisterServer()
{
    HRESULT hr = withFilterMapper([](IFilterMapper2 *mapper) -> HRESULT {
        auto instance = clsidString();

        return mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, instance.c_str(),
                                        CLSID_WebcamShareCam);
    });

    auto key = L"CLSID\\" + clsidString();
    LSTATUS status = RegDeleteTreeW(HKEY_CLASSES_ROOT, key.c_str());

    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(status);

    // Unregistering something that is not registered is fine.
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)? S_OK: hr;
}
