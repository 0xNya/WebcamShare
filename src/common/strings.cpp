#include "strings.h"

#include <atomic>

namespace wcs {

namespace {

struct Entry {
    const wchar_t *english;
    const wchar_t *russian;
};

const Entry kStrings[] = {
    // Tray status lines
    {L"Running: ", L"Работает: "},
    {L"Starting: ", L"Запуск: "},
    {L"Camera not found", L"Камера не найдена"},
    {L"Camera busy: ", L"Камера занята: "},
    {L"Error: ", L"Ошибка: "},
    {L"The virtual camera is not installed", L"Виртуальная камера не установлена"},
    // Status details
    {L"opening the camera", L"открытие камеры"},
    {L"the camera is used by another app", L"камера занята другой программой"},
    {L"camera access is blocked (Windows Settings → Privacy → Camera)",
     L"доступ к камере запрещён (Параметры Windows → Конфиденциальность → Камера)"},
    {L"the camera was disconnected", L"камера отключена"},
    {L"the camera sends no frames (it may be busy)", L"камера не отдаёт кадры (возможно, занята)"},
    {L"the camera stopped sending frames", L"камера перестала отдавать кадры"},
    {L"the camera has no suitable video mode", L"у камеры нет подходящего видеорежима"},
    {L"the selected camera mode is not available, using automatic",
     L"выбранный режим камеры недоступен, используется автоматический"},
    {L"another process already feeds the virtual camera",
     L"кадры в виртуальную камеру уже отправляет другой процесс"},
    {L"cannot create the frame channel", L"не удалось создать канал кадров"},
    // Menu
    {L"Camera", L"Камера"},
    {L"No cameras found", L"Камеры не найдены"},
    {L"Camera mode", L"Режим камеры"},
    {L"Automatic", L"Автоматически"},
    {L"(modes appear once the camera is open)", L"(режимы появятся, когда камера откроется)"},
    {L"Virtual camera resolution", L"Разрешение виртуальной камеры"},
    {L"Language / Язык", L"Language / Язык"},
    {L"System default", L"Как в системе"},
    {L"Restart capture", L"Перезапустить захват"},
    {L"Start with Windows", L"Запускать с Windows"},
    {L"Open log", L"Открыть журнал"},
    {L"Install the virtual camera…", L"Установить виртуальную камеру…"},
    {L"Exit", L"Выход"},
    {L"Resolution changed", L"Разрешение изменено"},
    {L"Reopen the camera in your apps to get %s.", L"Переоткройте камеру в программах, чтобы получить %s."},
    // Placeholder frames
    {L"Camera not found", L"Камера не найдена"},
    {L"Camera is used by another app", L"Камера занята другой программой"},
    {L"Camera disconnected", L"Камера отключена"},
    {L"No signal from the camera", L"Нет сигнала с камеры"},
    {L"WebcamShare is not running", L"Программа не запущена"},
    // Install / uninstall messages
    {L"Installation cancelled: administrator rights are required.",
     L"Установка отменена: нужны права администратора."},
    {L"Could not install the virtual camera. Details are in the log:\n",
     L"Не удалось установить виртуальную камеру. Подробности в журнале:\n"},
    {L"The \"WebcamShare 720p\" virtual camera is installed.\n"
     L"WebcamShare is running in the tray and will start with Windows.",
     L"Виртуальная камера «WebcamShare 720p» установлена.\n"
     L"Программа запущена в трее и будет запускаться вместе с Windows."},
    {L"Removing the virtual camera was cancelled: administrator rights are required. "
     L"Autostart is disabled.",
     L"Удаление драйвера отменено: нужны права администратора. Автозапуск отключён."},
    {L"The virtual camera was not removed completely. Details are in the log:\n",
     L"Драйвер удалён не полностью. Подробности в журнале:\n"},
    {L"The virtual camera is removed and autostart is disabled.\n"
     L"Settings and the log are kept in:\n",
     L"Виртуальная камера удалена, автозапуск отключён.\n"
     L"Настройки и журнал остались в папке:\n"},
};

static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == size_t(Str::Count), "string table size");

std::atomic<bool> g_russian {false};

}  // namespace

void setRussian(bool russian)
{
    g_russian = russian;
}

bool isRussian()
{
    return g_russian;
}

const wchar_t *tr(Str id)
{
    auto &entry = kStrings[size_t(id)];

    return g_russian? entry.russian: entry.english;
}

}  // namespace wcs
