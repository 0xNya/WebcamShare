// camsnap: saves one frame from the first real camera in a given native mode
// as a BMP, to compare what each mode shows (field of view, crop).
//   camsnap <FOURCC> <width> <height> <out.bmp>     e.g. camsnap NV12 1920 1080 a.bmp
// The camera must be free (quit WebcamShare first).
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static bool saveBmp(const wchar_t *path, const uint8_t *bgra, LONG pitch, UINT32 width, UINT32 height)
{
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    DWORD rowBytes = width * 3;
    DWORD padded = (rowBytes + 3) & ~3u;
    info.biSize = sizeof(info);
    info.biWidth = LONG(width);
    info.biHeight = LONG(height);  // bottom-up
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biSizeImage = padded * height;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    FILE *out = nullptr;

    if (_wfopen_s(&out, path, L"wb") || !out)
        return false;

    fwrite(&file, sizeof(file), 1, out);
    fwrite(&info, sizeof(info), 1, out);
    std::vector<uint8_t> row(padded);

    for (UINT32 y = 0; y < height; y++) {
        const uint8_t *src = bgra + size_t(height - 1 - y) * size_t(pitch);

        for (UINT32 x = 0; x < width; x++)
            memcpy(&row[x * 3], src + x * 4, 3);

        fwrite(row.data(), padded, 1, out);
    }

    fclose(out);

    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5) {
        printf("usage: camsnap <FOURCC> <width> <height> <out.bmp>\n");

        return 2;
    }

    UINT32 fourcc = 0;

    for (int i = 0; i < 4 && argv[1][i]; i++)
        fourcc |= UINT32(argv[1][i] & 0xFF) << (8 * i);

    UINT32 wantWidth = UINT32(_wtoi(argv[2]));
    UINT32 wantHeight = UINT32(_wtoi(argv[3]));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate **devices = nullptr;
    UINT32 count = 0;

    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)) || !count) {
        printf("no camera\n");

        return 1;
    }

    ComPtr<IMFMediaSource> source;
    HRESULT hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));

    for (UINT32 i = 0; i < count; i++)
        devices[i]->Release();

    CoTaskMemFree(devices);

    if (FAILED(hr)) {
        printf("activate failed 0x%08lx\n", hr);

        return 1;
    }

    ComPtr<IMFAttributes> readerAttributes;
    MFCreateAttributes(&readerAttributes, 1);
    readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttributes.Get(), &reader);

    if (FAILED(hr)) {
        printf("reader failed 0x%08lx\n", hr);

        return 1;
    }

    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    bool found = false;

    for (DWORD i = 0; !found; i++) {
        ComPtr<IMFMediaType> type;

        if (FAILED(reader->GetNativeMediaType(stream, i, &type)))
            break;

        GUID subtype;
        UINT32 width = 0, height = 0, num = 0, den = 1;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);

        if (subtype.Data1 == fourcc && width == wantWidth && height == wantHeight
            && std::fabs(double(num) / den - 30) < 1) {
            hr = reader->SetCurrentMediaType(stream, nullptr, type.Get());
            found = SUCCEEDED(hr);
        }
    }

    if (!found) {
        printf("mode not found\n");

        return 1;
    }

    ComPtr<IMFMediaType> output;
    MFCreateMediaType(&output);
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, wantWidth, wantHeight);
    hr = reader->SetCurrentMediaType(stream, nullptr, output.Get());

    if (FAILED(hr)) {
        printf("output type failed 0x%08lx\n", hr);

        return 1;
    }

    // Let auto exposure settle.
    ComPtr<IMFSample> sample;

    for (int frame = 0; frame < 45; frame++) {
        DWORD flags = 0;
        LONGLONG time = 0;
        sample.Reset();
        hr = reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample);

        if (FAILED(hr)) {
            printf("read failed 0x%08lx\n", hr);

            return 1;
        }
    }

    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    BYTE *data = nullptr;
    DWORD length = 0;
    buffer->Lock(&data, nullptr, &length);
    // RGB32 from MF is bottom-up with a positive default stride; flip to top-down.
    LONG pitch = LONG(wantWidth * 4);
    std::vector<uint8_t> topDown(length);

    for (UINT32 y = 0; y < wantHeight; y++)
        memcpy(&topDown[size_t(y) * pitch], data + size_t(y) * pitch, size_t(pitch));

    buffer->Unlock();
    bool ok = saveBmp(argv[4], topDown.data(), pitch, wantWidth, wantHeight);
    printf(ok? "saved %ls\n": "cannot write %ls\n", argv[4]);
    source->Shutdown();
    MFShutdown();

    return ok? 0: 1;
}
