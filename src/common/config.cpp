#include "config.h"

#include <windows.h>

namespace wcs {

namespace {

constexpr wchar_t kKey[] = L"Software\\WebcamShare";

DWORD readDword(const wchar_t *name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);

    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size)
        != ERROR_SUCCESS)
        return fallback;

    return value;
}

void writeDword(const wchar_t *name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &value, sizeof(value));
}

}  // namespace

bool isValidOutputSize(OutputSize size)
{
    for (auto &known: kOutputSizes)
        if (known.width == size.width && known.height == size.height)
            return true;

    return false;
}

OutputSize readOutputSize()
{
    OutputSize size = {readDword(L"OutputWidth", kDefaultOutputSize.width),
                       readDword(L"OutputHeight", kDefaultOutputSize.height)};

    return isValidOutputSize(size)? size: kDefaultOutputSize;
}

void writeOutputSize(OutputSize size)
{
    writeDword(L"OutputWidth", size.width);
    writeDword(L"OutputHeight", size.height);
}

Language readLanguage()
{
    auto value = readDword(L"Language", DWORD(Language::Auto));

    return value <= DWORD(Language::Russian)? Language(value): Language::Auto;
}

void writeLanguage(Language language)
{
    writeDword(L"Language", DWORD(language));
}

bool resolvesToRussian(Language language)
{
    if (language != Language::Auto)
        return language == Language::Russian;

    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

}  // namespace wcs
