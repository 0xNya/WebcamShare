// Settings shared by the app and the filter. The filter runs inside client
// processes and must know the output size before any frame arrives, so these
// live in HKCU\Software\WebcamShare (the app mirrors them from settings.json).
#pragma once

#include <cstdint>

namespace wcs {

struct OutputSize {
    uint32_t width;
    uint32_t height;
};

constexpr OutputSize kOutputSizes[] = {{1920, 1080}, {1280, 720}, {640, 360}};
constexpr OutputSize kDefaultOutputSize = {1280, 720};

bool isValidOutputSize(OutputSize size);
OutputSize readOutputSize();
void writeOutputSize(OutputSize size);

enum class Language : uint32_t { Auto = 0, English = 1, Russian = 2 };

Language readLanguage();
void writeLanguage(Language language);

// Auto resolves to Russian for a Russian Windows UI, English otherwise.
bool resolvesToRussian(Language language);

}  // namespace wcs
