// Append-only UTF-8 log in %APPDATA%\WebcamShare\log.txt, rotated by size
// (log.txt -> log.1.txt -> log.2.txt).
#pragma once

#include <string>

namespace wcs {

void logInit(const std::wstring &directory);
std::wstring logPath();

void logInfo(const wchar_t *format, ...);
void logWarning(const wchar_t *format, ...);
void logError(const wchar_t *format, ...);

}  // namespace wcs
