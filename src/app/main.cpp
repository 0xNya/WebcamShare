// WebcamShare: shares one real webcam as the "WebcamShare 720p" virtual
// camera (1280x720 @ 30) for any number of programs at once.
//
//   WebcamShare.exe              tray app (single instance)
//   WebcamShare.exe --install    install the virtual camera (UAC), enable autostart, start
//   WebcamShare.exe --uninstall  stop the app, disable autostart, remove the virtual camera (UAC)
#include <windows.h>
#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "common/strings.h"
#include "log.h"
#include "settings.h"
#include "system.h"
#include "tray.h"

using namespace wcs;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    logInit(appDataDirectory());

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode = argc > 1? argv[1]: L"";
    LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setRussian(resolvesToRussian(loadSettings().language));

    if (mode == L"--install")
        return runInstall();

    if (mode == L"--install-elevated")
        return runInstallElevated();

    if (mode == L"--uninstall")
        return runUninstall();

    if (mode == L"--uninstall-elevated")
        return runUninstallElevated();

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: nothing to do, the tray icon is there.
        CloseHandle(mutex);

        return 0;
    }

    logInfo(L"WebcamShare started: %s", exePath().c_str());
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    int code;

    {
        TrayApp app;
        code = app.run(instance);
    }

    MFShutdown();
    CoUninitialize();
    CloseHandle(mutex);

    return code;
}
