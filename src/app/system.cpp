#include "system.h"
#include "log.h"
#include "resource.h"
#include "common/strings.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <vector>

namespace wcs {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kFilterClsidKey[] = L"CLSID\\{AAD9A7D0-FC31-43B2-A807-FB7BA56DF5D3}\\InprocServer32";

struct DriverFile {
    int resource;
    const wchar_t *arch;
    const wchar_t *regsvr;  // regsvr32 of the matching bitness
};

const DriverFile kDriverFiles[] = {
    {IDR_VCAM_X64, L"x64", L"\\System32\\regsvr32.exe"},
    {IDR_VCAM_X86, L"x86", L"\\SysWOW64\\regsvr32.exe"},
};

std::wstring windowsDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetWindowsDirectoryW(path, MAX_PATH);

    return path;
}

std::wstring driverPath(const DriverFile &file)
{
    return driverDirectory() + L"\\" + file.arch + L"\\WebcamShareCam.dll";
}

int runAndWait(const std::wstring &program, const std::wstring &arguments)
{
    std::wstring commandLine = L"\"" + program + L"\" " + arguments;
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};

    if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return -1;

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return int(code);
}

// A DLL loaded by a running camera client cannot be deleted, but it can be
// renamed and removed at the next reboot.
bool removeFile(const std::wstring &path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES || DeleteFileW(path.c_str()))
        return true;

    auto old = path + L"." + std::to_wstring(GetTickCount64()) + L".old";

    if (!MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
        return false;

    MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    logInfo(L"%s is in use, it will be deleted at reboot", path.c_str());

    return true;
}

void removeStaleFiles(const std::wstring &directory)
{
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((directory + L"\\*.old").c_str(), &data);

    if (find == INVALID_HANDLE_VALUE)
        return;

    do
        DeleteFileW((directory + L"\\" + data.cFileName).c_str());
    while (FindNextFileW(find, &data));

    FindClose(find);
}

bool extractResource(int id, const std::wstring &path)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = resource? LoadResource(nullptr, resource): nullptr;
    auto data = loaded? LockResource(loaded): nullptr;
    DWORD size = resource? SizeofResource(nullptr, resource): 0;

    if (!data || !size)
        return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);

    return ok;
}

void message(const std::wstring &text, UINT icon)
{
    MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | icon | MB_SETFOREGROUND);
}

}  // namespace

std::wstring exePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);

    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));

        if (length < buffer.size())
            return std::wstring(buffer.data(), length);

        buffer.resize(buffer.size() * 2);
    }
}

bool isElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)
                    && GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)
                    && elevation.TokenIsElevated;

    if (token)
        CloseHandle(token);

    return elevated;
}

bool isAutostartEnabled()
{
    wchar_t value[2048] = {};
    DWORD size = sizeof(value);

    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, value, &size)
        != ERROR_SUCCESS)
        return false;

    // Enabled only if it points at this exe (the exe may have been moved).
    return _wcsicmp(value, (L"\"" + exePath() + L"\"").c_str()) == 0;
}

bool setAutostart(bool enabled)
{
    LSTATUS status;

    if (enabled) {
        auto command = L"\"" + exePath() + L"\"";
        status = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ, command.c_str(),
                                 DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);

        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }

    if (status != ERROR_SUCCESS)
        logError(L"cannot %s autostart: %ld", enabled? L"enable": L"disable", status);
    else
        logInfo(L"autostart %s", enabled? L"enabled": L"disabled");

    return status == ERROR_SUCCESS;
}

std::wstring driverDirectory()
{
    PWSTR path = nullptr;
    std::wstring directory;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path)))
        directory = std::wstring(path) + L"\\WebcamShare";

    CoTaskMemFree(path);

    return directory;
}

bool isDriverInstalled()
{
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);

    if (RegGetValueW(HKEY_CLASSES_ROOT, kFilterClsidKey, nullptr, RRF_RT_REG_SZ, nullptr, path, &size)
        != ERROR_SUCCESS)
        return false;

    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

int runElevated(const wchar_t *arguments)
{
    auto exe = exePath();
    SHELLEXECUTEINFOW info = {sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments;
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info) || !info.hProcess) {
        logWarning(L"elevation refused or failed: %lu", GetLastError());

        return -1;
    }

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);

    return int(code);
}

int runInstallElevated()
{
    auto directory = driverDirectory();
    logInfo(L"installing the virtual camera into %s", directory.c_str());
    CreateDirectoryW(directory.c_str(), nullptr);

    for (auto &file: kDriverFiles) {
        auto subdirectory = directory + L"\\" + file.arch;
        auto path = driverPath(file);
        CreateDirectoryW(subdirectory.c_str(), nullptr);
        removeStaleFiles(subdirectory);

        if (!removeFile(path) || !extractResource(file.resource, path)) {
            logError(L"cannot write %s: %lu", path.c_str(), GetLastError());

            return 2;
        }

        int code = runAndWait(windowsDirectory() + file.regsvr, L"/s \"" + path + L"\"");

        if (code != 0) {
            logError(L"regsvr32 %s failed: %d", path.c_str(), code);

            return 3;
        }

        logInfo(L"registered %s", path.c_str());
    }

    return 0;
}

int runUninstallElevated()
{
    int result = 0;

    for (auto &file: kDriverFiles) {
        auto path = driverPath(file);

        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        int code = runAndWait(windowsDirectory() + file.regsvr, L"/s /u \"" + path + L"\"");

        if (code != 0) {
            logError(L"regsvr32 /u %s failed: %d", path.c_str(), code);
            result = 3;
        }

        removeFile(path);
        RemoveDirectoryW((driverDirectory() + L"\\" + file.arch).c_str());
    }

    RemoveDirectoryW(driverDirectory().c_str());
    logInfo(L"virtual camera uninstalled");

    return result;
}

int runInstall()
{
    int code = isElevated()? runInstallElevated(): runElevated(L"--install-elevated");

    if (code != 0) {
        message(code < 0? std::wstring(tr(Str::MsgInstallCancelled)): tr(Str::MsgInstallFailed) + logPath(),
                MB_ICONERROR);

        return 1;
    }

    setAutostart(true);

    // Start the tray app unless it is already running.
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);

    if (mutex) {
        CloseHandle(mutex);
    } else {
        auto exe = exePath();
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    message(tr(Str::MsgInstalled), MB_ICONINFORMATION);

    return 0;
}

int runUninstall()
{
    // Ask a running instance to quit and wait until it has.
    if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, kQuitEvent)) {
        SetEvent(quit);
        CloseHandle(quit);

        for (int i = 0; i < 50; i++) {
            HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);

            if (!mutex)
                break;

            CloseHandle(mutex);
            Sleep(100);
        }
    }

    setAutostart(false);
    int code = isElevated()? runUninstallElevated(): runElevated(L"--uninstall-elevated");

    if (code != 0) {
        message(code < 0? std::wstring(tr(Str::MsgUninstallCancelled)): tr(Str::MsgUninstallPartial) + logPath(),
                MB_ICONWARNING);

        return 1;
    }

    message(tr(Str::MsgUninstalled) + logPath().substr(0, logPath().rfind(L'\\')), MB_ICONINFORMATION);

    return 0;
}

}  // namespace wcs
