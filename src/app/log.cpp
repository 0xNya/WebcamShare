#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wcs {

namespace {

constexpr long long kMaxLogSize = 1024 * 1024;
constexpr int kKeptLogs = 2;  // log.1.txt, log.2.txt

std::mutex g_mutex;
std::wstring g_directory;
HANDLE g_file = INVALID_HANDLE_VALUE;

std::wstring rotatedName(int index)
{
    return g_directory + L"\\log." + std::to_wstring(index) + L".txt";
}

void openFile()
{
    // Shared for writing: the tray app, --install and its elevated half all
    // log at once. FILE_APPEND_DATA keeps each WriteFile an atomic append.
    g_file = CreateFileW(logPath().c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void rotateIfNeeded()
{
    LARGE_INTEGER size = {};

    if (g_file == INVALID_HANDLE_VALUE || !GetFileSizeEx(g_file, &size) || size.QuadPart < kMaxLogSize)
        return;

    CloseHandle(g_file);
    DeleteFileW(rotatedName(kKeptLogs).c_str());

    for (int i = kKeptLogs - 1; i >= 1; i--)
        MoveFileExW(rotatedName(i).c_str(), rotatedName(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);

    MoveFileExW(logPath().c_str(), rotatedName(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    openFile();
}

void write(const char *level, const wchar_t *format, va_list args)
{
    wchar_t message[1024];
    _vsnwprintf_s(message, _TRUNCATE, format, args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t line[1200];
    int length = _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%S] %s\r\n",
                              now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                              now.wSecond, now.wMilliseconds, level, message);

    if (length < 0)
        length = int(wcslen(line));

    char utf8[3600];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, length, utf8, sizeof(utf8), nullptr, nullptr);

#ifdef _DEBUG
    OutputDebugStringW(line);
#endif

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(g_file, utf8, DWORD(bytes), &written, nullptr);
    rotateIfNeeded();
}

}  // namespace

void logInit(const std::wstring &directory)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_directory = directory;
    CreateDirectoryW(directory.c_str(), nullptr);
    openFile();
    rotateIfNeeded();
}

std::wstring logPath()
{
    return g_directory + L"\\log.txt";
}

void logInfo(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    write("INFO", format, args);
    va_end(args);
}

void logWarning(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    write("WARN", format, args);
    va_end(args);
}

void logError(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    write("ERROR", format, args);
    va_end(args);
}

}  // namespace wcs
