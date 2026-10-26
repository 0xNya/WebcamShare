// Tray icon, its menu and the hidden window that owns them.
#pragma once

#include <windows.h>

#include <memory>
#include <vector>

#include "capture.h"
#include "settings.h"

namespace wcs {

class TrayApp {
public:
    TrayApp() = default;
    ~TrayApp();

    // Runs the message loop until "Exit" or the quit event.
    int run(HINSTANCE instance);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    void addIcon();
    void updateIcon();
    void showMenu();
    HMENU buildModeMenu();
    void onCommand(UINT id);
    void notify(const std::wstring &title, const std::wstring &text);
    std::wstring statusLine(const CaptureStatus &status, std::wstring *detail) const;

    HWND m_window = nullptr;
    UINT m_taskbarCreated = 0;
    HDEVNOTIFY m_deviceNotify = nullptr;
    HICON m_icons[3] = {};  // ok, warning, error
    std::unique_ptr<Capture> m_capture;
    Settings m_settings;
    std::vector<CameraInfo> m_menuCameras;
    std::vector<CameraMode> m_menuModes;
    bool m_driverInstalled = false;
};

}  // namespace wcs
