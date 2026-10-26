#include "settings.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace wcs {

namespace {

std::wstring fromUtf8(const std::string &text)
{
    if (text.empty())
        return {};

    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length);

    return result;
}

std::string toUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};

    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string result(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length,
                        nullptr, nullptr);

    return result;
}

class Parser {
public:
    explicit Parser(const std::string &text): m_text(text) {}

    std::optional<std::map<std::wstring, std::wstring>> object()
    {
        std::map<std::wstring, std::wstring> values;
        skipSpace();

        if (!consume('{'))
            return std::nullopt;

        skipSpace();

        if (consume('}'))
            return finish(values);

        for (;;) {
            std::string key;
            std::string value;
            skipSpace();

            if (!string(key))
                return std::nullopt;

            skipSpace();

            if (!consume(':'))
                return std::nullopt;

            skipSpace();

            if (!scalar(value))
                return std::nullopt;

            values[fromUtf8(key)] = fromUtf8(value);
            skipSpace();

            if (consume(','))
                continue;

            if (consume('}'))
                return finish(values);

            return std::nullopt;
        }
    }

private:
    std::optional<std::map<std::wstring, std::wstring>> finish(
            std::map<std::wstring, std::wstring> &values)
    {
        skipSpace();

        if (m_pos != m_text.size())
            return std::nullopt;

        return values;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() && strchr(" \t\r\n", m_text[m_pos]) && m_text[m_pos])
            m_pos++;

        // Tolerate a UTF-8 BOM at the start.
        if (m_pos == 0 && m_text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            m_pos = 3;
            skipSpace();
        }
    }

    bool consume(char c)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == c) {
            m_pos++;

            return true;
        }

        return false;
    }

    // Strings, plus true/false/null/numbers kept as their literal text.
    bool scalar(std::string &out)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == '"')
            return string(out);

        size_t start = m_pos;

        while (m_pos < m_text.size() && (isalnum(uint8_t(m_text[m_pos])) || strchr("+-.", m_text[m_pos])))
            m_pos++;

        out = m_text.substr(start, m_pos - start);

        return !out.empty();
    }

    bool string(std::string &out)
    {
        if (!consume('"'))
            return false;

        while (m_pos < m_text.size()) {
            char c = m_text[m_pos++];

            if (c == '"')
                return true;

            if (c != '\\') {
                out += c;

                continue;
            }

            if (m_pos >= m_text.size())
                return false;

            char e = m_text[m_pos++];

            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (m_pos + 4 > m_text.size())
                    return false;

                unsigned code = std::stoul(m_text.substr(m_pos, 4), nullptr, 16);
                m_pos += 4;

                // Surrogate pair.
                if (code >= 0xD800 && code < 0xDC00 && m_text.compare(m_pos, 2, "\\u") == 0
                    && m_pos + 6 <= m_text.size()) {
                    unsigned low = std::stoul(m_text.substr(m_pos + 2, 4), nullptr, 16);
                    m_pos += 6;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }

                wchar_t units[2];
                int n = 1;

                if (code >= 0x10000) {
                    units[0] = wchar_t(0xD800 + ((code - 0x10000) >> 10));
                    units[1] = wchar_t(0xDC00 + ((code - 0x10000) & 0x3FF));
                    n = 2;
                } else {
                    units[0] = wchar_t(code);
                }

                out += toUtf8(std::wstring(units, size_t(n)));

                break;
            }
            default:
                return false;
            }
        }

        return false;
    }

    const std::string &m_text;
    size_t m_pos = 0;
};

std::string escape(const std::string &text)
{
    std::string out;

    for (char c: text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x", unsigned(uint8_t(c)));
                out += buffer;
            } else {
                out += c;
            }
        }
    }

    return out;
}

std::wstring settingsPath()
{
    return appDataDirectory() + L"\\settings.json";
}

}  // namespace

std::optional<std::map<std::wstring, std::wstring>> parseFlatJson(const std::string &utf8)
{
    try {
        return Parser(utf8).object();
    } catch (...) {
        return std::nullopt;  // malformed \u escape
    }
}

std::string serializeFlatJson(const std::map<std::wstring, std::wstring> &values)
{
    std::string out = "{\n";
    size_t i = 0;

    for (auto &[key, value]: values) {
        out += "  \"" + escape(toUtf8(key)) + "\": \"" + escape(toUtf8(value)) + "\"";
        out += ++i < values.size()? ",\n": "\n";
    }

    return out + "}\n";
}

std::wstring appDataDirectory()
{
    PWSTR path = nullptr;
    std::wstring directory;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)))
        directory = std::wstring(path) + L"\\WebcamShare";

    CoTaskMemFree(path);

    return directory;
}

Settings loadSettings()
{
    Settings settings;
    std::ifstream file(settingsPath(), std::ios::binary);

    if (!file)
        return settings;

    std::stringstream buffer;
    buffer << file.rdbuf();
    auto values = parseFlatJson(buffer.str());

    if (!values) {
        logWarning(L"settings.json is malformed, using defaults");

        return settings;
    }

    settings.cameraName = (*values)[L"cameraName"];
    settings.cameraLink = (*values)[L"cameraLink"];
    settings.cameraMode = (*values)[L"cameraMode"];

    unsigned width = 0, height = 0;

    if (swscanf_s((*values)[L"outputSize"].c_str(), L"%ux%u", &width, &height) == 2
        && isValidOutputSize({width, height}))
        settings.output = {width, height};

    auto language = (*values)[L"language"];
    settings.language = language == L"en"? Language::English:
                        language == L"ru"? Language::Russian: Language::Auto;

    return settings;
}

bool saveSettings(const Settings &settings)
{
    CreateDirectoryW(appDataDirectory().c_str(), nullptr);
    auto path = settingsPath();
    auto temporary = path + L".tmp";
    auto json = serializeFlatJson({
        {L"cameraName", settings.cameraName},
        {L"cameraLink", settings.cameraLink},
        {L"cameraMode", settings.cameraMode},
        {L"outputSize", std::to_wstring(settings.output.width) + L"x"
                            + std::to_wstring(settings.output.height)},
        {L"language", settings.language == Language::English? L"en":
                      settings.language == Language::Russian? L"ru": L"auto"},
    });

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

        if (!file.write(json.data(), std::streamsize(json.size())))
            return false;
    }

    // Replace atomically so a crash never leaves a half-written file.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logError(L"cannot save settings: %lu", GetLastError());

        return false;
    }

    return true;
}

}  // namespace wcs
