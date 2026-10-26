// %APPDATA%\WebcamShare\settings.json: a flat JSON object of strings.
#pragma once

#include <map>
#include <optional>
#include <string>

#include "common/config.h"

namespace wcs {

struct Settings {
    // Selected source camera; empty = first available camera.
    std::wstring cameraName;
    std::wstring cameraLink;  // MF symbolic link, preferred over the name
    std::wstring cameraMode;  // CameraMode::text(), empty = automatic
    OutputSize output = kDefaultOutputSize;
    Language language = Language::Auto;
};

std::wstring appDataDirectory();
Settings loadSettings();
bool saveSettings(const Settings &settings);

// Exposed for tests: parse/serialize a flat {"key": "value"} object.
std::optional<std::map<std::wstring, std::wstring>> parseFlatJson(const std::string &utf8);
std::string serializeFlatJson(const std::map<std::wstring, std::wstring> &values);

}  // namespace wcs
