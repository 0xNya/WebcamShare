#include "tray.h"
#include "log.h"
#include "system.h"

#include <dbt.h>
#include <ks.h>
#include <ksmedia.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace wcs {

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_CAPTURE_STATUS = WM_APP + 2;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    IdRestart = 100,
    IdAutostart,
    IdOpenLog,
    IdInstallDriver,
    IdExit,
    IdLanguageAuto,
    IdLanguageEnglish,
    IdLanguageRussian,
    IdModeAuto,
    IdCameraFirst = 1000,  // + index into m_menuCameras
    IdModeFirst = 2000,    // + index into m_menuModes
    IdOutputFirst = 3000,  // + index into kOutputSizes
};

std::wstring sizeText(uint32_t width, uint32_t height)
{
    return std::to_wstring(width) + L"x" + std::to_wstring(height);
}

enum IconIndex { IconOk, IconWarning, IconError };

// Video camera glyph with a status dot, drawn into a 32-bit ARGB icon.
HICON makeIcon(int size, COLORREF dot)
{
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);

    if (!color)
        return nullptr;

    auto pixels = static_cast<uint32_t *>(bits);
    double s = size;
    double dotX = 0.74 * s, dotY = 0.74 * s, dotR = 0.25 * s;

    auto argb = [](double alpha, int r, int g, int b) {
        auto a = uint32_t(alpha * 255 + 0.5);
        // Premultiplied, as icons with alpha expect.
        return a << 24 | uint32_t(r * a / 255) << 16 | uint32_t(g * a / 255) << 8 | uint32_t(b * a / 255);
    };

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double px = x + 0.5, py = y + 0.5;
            uint32_t value = 0;
            bool body = px >= 0.06 * s && px <= 0.62 * s && py >= 0.24 * s && py <= 0.68 * s;
            double lensHalf = 0.08 * s + (px - 0.62 * s) * 0.7;
            bool lens = px > 0.62 * s && px <= 0.94 * s && std::fabs(py - 0.46 * s) <= lensHalf;

            if (body || lens)
                value = argb(1, 235, 235, 240);

            double distance = std::hypot(px - dotX, py - dotY);

            if (distance <= dotR + 1.5) {
                double ring = std::clamp(dotR + 1.5 - distance, 0.0, 1.0);  // dark outline
                double fill = std::clamp(dotR - distance, 0.0, 1.0);

                auto mix = [&](BYTE channel) { return int(channel * fill + 30 * (1 - fill)); };

                if (fill > 0)
                    value = argb(1, mix(GetRValue(dot)), mix(GetGValue(dot)), mix(GetBValue(dot)));
                else if (ring > 0)
                    value = argb(std::max(ring, value? 1.0: 0.0), 30, 30, 30);
            }

            pixels[y * size + x] = value;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);

    return icon;
}

void appendItem(HMENU menu, UINT id, const std::wstring &text, UINT flags = 0)
{
    AppendMenuW(menu, MF_STRING | flags, id, text.c_str());
}

}  // namespace

TrayApp::~TrayApp()
{
    for (auto icon: m_icons)
        if (icon)
            DestroyIcon(icon);
}

int TrayApp::run(HINSTANCE instance)
{
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"WebcamShareTray";
    RegisterClassExW(&windowClass);

    // A regular top-level (not message-only) window: it receives the
    // TaskbarCreated broadcast and WM_DEVICECHANGE.
    m_window = CreateWindowExW(0, windowClass.lpszClassName, kAppName, WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, instance, this);

    if (!m_window)
        return 1;

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    m_icons[IconOk] = makeIcon(iconSize, RGB(60, 200, 90));
    m_icons[IconWarning] = makeIcon(iconSize, RGB(245, 180, 40));
    m_icons[IconError] = makeIcon(iconSize, RGB(230, 60, 50));

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {sizeof(filter)};
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = KSCATEGORY_VIDEO_CAMERA;
    m_deviceNotify = RegisterDeviceNotificationW(m_window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    m_driverInstalled = isDriverInstalled();

    if (!m_driverInstalled)
        logWarning(L"the virtual camera driver is not installed");

    m_settings = loadSettings();
    // The filter reads these from the registry inside client processes.
    writeOutputSize(m_settings.output);
    writeLanguage(m_settings.language);
    setRussian(resolvesToRussian(m_settings.language));

    m_capture = std::make_unique<Capture>(m_window, WM_CAPTURE_STATUS);
    m_capture->start({m_settings.cameraName, m_settings.cameraLink},
                     CameraMode::parse(m_settings.cameraMode), m_settings.output);
    addIcon();

    // We are the only instance (mutex), so a signaled quit event is stale:
    // left over from stopping a previous instance whose event someone still holds.
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
    ResetEvent(quit);
    MSG msg = {};
    bool running = true;

    while (running) {
        DWORD wait = MsgWaitForMultipleObjects(1, &quit, FALSE, INFINITE, QS_ALLINPUT);

        if (wait == WAIT_OBJECT_0) {
            logInfo(L"quit requested by another process");
            DestroyWindow(m_window);
        }

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;

                break;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CloseHandle(quit);

    return int(msg.wParam);
}

LRESULT CALLBACK TrayApp::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
    }

    auto self = reinterpret_cast<TrayApp *>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (self && self->m_window)
        return self->handle(message, wParam, lParam);

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayApp::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == m_taskbarCreated && m_taskbarCreated) {
        addIcon();  // Explorer restarted

        return 0;
    }

    switch (message) {
    case WM_TRAY:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            showMenu();

            break;
        }

        return 0;

    case WM_CAPTURE_STATUS:
        updateIcon();

        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            logInfo(L"camera %s", wParam == DBT_DEVICEARRIVAL? L"connected": L"disconnected");
            m_capture->devicesChanged();
        }

        return TRUE;

    case WM_COMMAND:
        onCommand(LOWORD(wParam));

        return 0;

    case WM_DESTROY: {
        m_capture->stop();

        if (m_deviceNotify)
            UnregisterDeviceNotification(m_deviceNotify);

        NOTIFYICONDATAW data = {sizeof(data)};
        data.hWnd = m_window;
        data.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        logInfo(L"WebcamShare stopped");
        PostQuitMessage(0);

        return 0;
    }
    }

    return DefWindowProcW(m_window, message, wParam, lParam);
}

std::wstring TrayApp::statusLine(const CaptureStatus &status, std::wstring *detail) const
{
    // Second line: the reason (translated) and/or the raw detail.
    std::wstring reason = status.reason == Str::Count? L"": tr(status.reason);
    *detail = reason.empty()? status.detail:
              status.detail.empty()? reason: status.detail + L" — " + reason;

    switch (status.state) {
    case CaptureState::Running:
        return tr(Str::StatusRunning) + status.camera;
    case CaptureState::Starting:
        return tr(Str::StatusStarting) + status.camera;
    case CaptureState::NoCamera:
        return status.camera.empty()? std::wstring(tr(Str::StatusNoCamera)):
                                      tr(Str::StatusNoCamera) + (L": " + status.camera);
    case CaptureState::CameraBusy:
        return tr(Str::StatusBusy) + status.camera;
    case CaptureState::ChannelBusy:
    case CaptureState::Error:
        break;
    }

    auto line = tr(Str::StatusError) + (reason.empty()? status.detail: reason);
    *detail = reason.empty()? L"": status.detail;

    return line;
}

void TrayApp::addIcon()
{
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = m_window;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_TRAY;
    data.hIcon = m_icons[IconWarning];
    wcscpy_s(data.szTip, kAppName);
    Shell_NotifyIconW(NIM_DELETE, &data);
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    updateIcon();
}

void TrayApp::updateIcon()
{
    auto status = m_capture->status();
    std::wstring detail;
    auto line = statusLine(status, &detail);
    int icon = status.state == CaptureState::Running? IconOk:
               status.state == CaptureState::Error || status.state == CaptureState::ChannelBusy?
                   IconError: IconWarning;

    if (!m_driverInstalled) {
        icon = IconError;
        line = tr(Str::DriverMissing);
    }

    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = m_window;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = m_icons[icon];
    swprintf_s(data.szTip, L"%s\n%s", kAppName, line.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayApp::notify(const std::wstring &title, const std::wstring &text)
{
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = m_window;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// "Camera mode": Automatic, then per 16:9 resolution the 30/60 fps modes of
// the uncompressed and MJPEG formats (4:3 modes crop the field of view).
HMENU TrayApp::buildModeMenu()
{
    HMENU menu = CreatePopupMenu();
    auto current = CameraMode::parse(m_settings.cameraMode);
    m_menuModes.clear();
    appendItem(menu, IdModeAuto, tr(Str::MenuModeAuto));
    UINT checked = IdModeAuto;

    auto formats = m_capture->modes();
    std::vector<std::pair<uint32_t, uint32_t>> sizes;

    for (auto &f: formats)
        if (f.width * 9 == f.height * 16
            && std::find(sizes.begin(), sizes.end(), std::make_pair(f.width, f.height)) == sizes.end())
            sizes.push_back({f.width, f.height});

    std::sort(sizes.begin(), sizes.end(), [](auto &a, auto &b) { return a.first > b.first; });

    if (sizes.empty())
        appendItem(menu, 0, tr(Str::MenuModeUnknown), MF_GRAYED);
    else
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const uint32_t fourccs[] = {0x3231564E /* NV12 */, 0x32595559 /* YUY2 */, 0x47504A4D /* MJPG */};

    for (auto &[width, height]: sizes) {
        HMENU sizeMenu = CreatePopupMenu();

        for (auto fourcc: fourccs) {
            for (int fps: {30, 60}) {
                CameraMode mode = {fourcc, width, height, fps};
                bool available = std::any_of(formats.begin(), formats.end(),
                                             [&](const NativeFormat &f) { return mode.matches(f); });

                if (!available)
                    continue;

                UINT id = IdModeFirst + UINT(m_menuModes.size());
                m_menuModes.push_back(mode);
                appendItem(sizeMenu, id, fourccName(fourcc) + L" · " + std::to_wstring(fps) + L" fps");

                if (!current.isAuto() && current.text() == mode.text())
                    checked = id;
            }
        }

        AppendMenuW(menu, MF_POPUP, UINT_PTR(sizeMenu), sizeText(width, height).c_str());
    }

    // Radio marks work across submenus by command id.
    if (checked == IdModeAuto)
        CheckMenuRadioItem(menu, IdModeAuto, IdModeAuto, IdModeAuto, MF_BYCOMMAND);
    else
        CheckMenuItem(menu, checked, MF_BYCOMMAND | MF_CHECKED);

    return menu;
}

void TrayApp::showMenu()
{
    auto status = m_capture->status();
    std::wstring detail;
    auto line = statusLine(status, &detail);
    HMENU menu = CreatePopupMenu();

    if (!m_driverInstalled)
        appendItem(menu, 0, tr(Str::DriverMissing), MF_GRAYED);

    appendItem(menu, 0, line, MF_GRAYED);

    if (!detail.empty())
        appendItem(menu, 0, detail, MF_GRAYED);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Camera
    HMENU cameras = CreatePopupMenu();
    m_menuCameras = listCameras();

    // Same rule as Capture::pickCamera: link, then name, else the first one.
    int selected = -1;

    if (m_settings.cameraLink.empty() && m_settings.cameraName.empty())
        selected = m_menuCameras.empty()? -1: 0;

    for (size_t i = 0; i < m_menuCameras.size() && selected < 0; i++)
        if (_wcsicmp(m_menuCameras[i].link.c_str(), m_settings.cameraLink.c_str()) == 0)
            selected = int(i);

    for (size_t i = 0; i < m_menuCameras.size() && selected < 0; i++)
        if (m_menuCameras[i].name == m_settings.cameraName)
            selected = int(i);

    for (size_t i = 0; i < m_menuCameras.size(); i++)
        appendItem(cameras, IdCameraFirst + UINT(i), m_menuCameras[i].name);

    if (selected >= 0)
        CheckMenuRadioItem(cameras, IdCameraFirst, IdCameraFirst + UINT(m_menuCameras.size()) - 1,
                           IdCameraFirst + UINT(selected), MF_BYCOMMAND);

    if (m_menuCameras.empty())
        appendItem(cameras, 0, tr(Str::MenuNoCameras), MF_GRAYED);

    AppendMenuW(menu, MF_POPUP, UINT_PTR(cameras), tr(Str::MenuCamera));
    AppendMenuW(menu, MF_POPUP, UINT_PTR(buildModeMenu()), tr(Str::MenuCameraMode));

    // Virtual camera resolution
    HMENU sizes = CreatePopupMenu();
    UINT sizeCount = UINT(std::size(kOutputSizes));

    for (UINT i = 0; i < sizeCount; i++) {
        appendItem(sizes, IdOutputFirst + i, sizeText(kOutputSizes[i].width, kOutputSizes[i].height));

        if (kOutputSizes[i].width == m_settings.output.width && kOutputSizes[i].height == m_settings.output.height)
            CheckMenuRadioItem(sizes, IdOutputFirst, IdOutputFirst + sizeCount - 1, IdOutputFirst + i,
                               MF_BYCOMMAND);
    }

    AppendMenuW(menu, MF_POPUP, UINT_PTR(sizes), tr(Str::MenuOutputSize));

    // Language
    HMENU languages = CreatePopupMenu();
    appendItem(languages, IdLanguageAuto, tr(Str::MenuLanguageAuto));
    appendItem(languages, IdLanguageEnglish, L"English");
    appendItem(languages, IdLanguageRussian, L"Русский");
    CheckMenuRadioItem(languages, IdLanguageAuto, IdLanguageRussian,
                       IdLanguageAuto + UINT(m_settings.language), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(languages), tr(Str::MenuLanguage));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendItem(menu, IdRestart, tr(Str::MenuRestart));
    appendItem(menu, IdAutostart, tr(Str::MenuAutostart), isAutostartEnabled()? MF_CHECKED: 0);
    appendItem(menu, IdOpenLog, tr(Str::MenuOpenLog));

    if (!m_driverInstalled)
        appendItem(menu, IdInstallDriver, tr(Str::MenuInstallDriver));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendItem(menu, IdExit, tr(Str::MenuExit));

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);  // so the menu closes when clicking elsewhere
    UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                               cursor.x, cursor.y, m_window, nullptr);
    DestroyMenu(menu);
    PostMessageW(m_window, WM_NULL, 0, 0);

    if (id)
        onCommand(id);
}

void TrayApp::onCommand(UINT id)
{
    if (id >= IdCameraFirst && id < IdCameraFirst + m_menuCameras.size()) {
        auto &camera = m_menuCameras[id - IdCameraFirst];
        logInfo(L"camera selected: %s", camera.name.c_str());
        m_settings.cameraName = camera.name;
        m_settings.cameraLink = camera.link;
        m_settings.cameraMode.clear();  // modes belong to a camera
        saveSettings(m_settings);
        m_capture->select(camera);

        return;
    }

    if (id == IdModeAuto || (id >= IdModeFirst && id < IdModeFirst + m_menuModes.size())) {
        CameraMode mode = id == IdModeAuto? CameraMode {}: m_menuModes[id - IdModeFirst];
        logInfo(L"camera mode selected: %s", mode.isAuto()? L"automatic": mode.text().c_str());
        m_settings.cameraMode = mode.text();
        saveSettings(m_settings);
        m_capture->setMode(mode);

        return;
    }

    if (id >= IdOutputFirst && id < IdOutputFirst + std::size(kOutputSizes)) {
        auto size = kOutputSizes[id - IdOutputFirst];

        if (size.width == m_settings.output.width && size.height == m_settings.output.height)
            return;

        logInfo(L"output size selected: %ux%u", size.width, size.height);
        m_settings.output = size;
        saveSettings(m_settings);
        writeOutputSize(size);
        m_capture->setOutputSize(size);

        wchar_t text[256];
        swprintf_s(text, tr(Str::NoticeSizeText), sizeText(size.width, size.height).c_str());
        notify(tr(Str::NoticeSizeTitle), text);

        return;
    }

    if (id >= IdLanguageAuto && id <= IdLanguageRussian) {
        m_settings.language = Language(id - IdLanguageAuto);
        saveSettings(m_settings);
        writeLanguage(m_settings.language);
        setRussian(resolvesToRussian(m_settings.language));
        updateIcon();

        return;
    }

    switch (id) {
    case IdRestart:
        logInfo(L"restart requested from the menu");
        m_capture->restart();

        break;

    case IdAutostart:
        setAutostart(!isAutostartEnabled());

        break;

    case IdOpenLog:
        ShellExecuteW(nullptr, L"open", logPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        break;

    case IdInstallDriver:
        if (runElevated(L"--install-elevated") == 0)
            logInfo(L"virtual camera installed from the tray");

        m_driverInstalled = isDriverInstalled();
        updateIcon();

        break;

    case IdExit:
        DestroyWindow(m_window);

        break;
    }
}

}  // namespace wcs
