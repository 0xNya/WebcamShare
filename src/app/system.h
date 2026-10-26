// Autostart, single-instance names and the virtual camera driver
// (install/uninstall need admin and run in an elevated copy of the exe).
#pragma once

#include <string>

namespace wcs {

constexpr wchar_t kAppName[] = L"WebcamShare";
constexpr wchar_t kInstanceMutex[] = L"Local\\WebcamShare.Instance";
constexpr wchar_t kQuitEvent[] = L"Local\\WebcamShare.Quit";

std::wstring exePath();
bool isElevated();

bool isAutostartEnabled();
bool setAutostart(bool enabled);

bool isDriverInstalled();
std::wstring driverDirectory();

// Command-line modes; return the process exit code.
int runInstall();            // UI side: elevates, then enables autostart and starts the app
int runInstallElevated();    // extracts and registers the filter DLLs
int runUninstall();          // UI side: stops the app, removes autostart, elevates
int runUninstallElevated();  // unregisters and deletes the filter DLLs

// Starts the elevated half of install/uninstall and waits for it.
int runElevated(const wchar_t *arguments);

}  // namespace wcs
