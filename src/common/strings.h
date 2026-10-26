// User-visible strings in English and Russian.
#pragma once

namespace wcs {

enum class Str {
    // Tray status lines
    StatusRunning,
    StatusStarting,
    StatusNoCamera,
    StatusBusy,
    StatusError,
    DriverMissing,
    // Status details
    DetailOpening,
    DetailBusy,
    DetailAccessDenied,
    DetailUnplugged,
    DetailNoFrames,
    DetailStalled,
    DetailNoMode,
    DetailModeMissing,
    DetailChannelBusy,
    DetailChannelError,
    // Menu
    MenuCamera,
    MenuNoCameras,
    MenuCameraMode,
    MenuModeAuto,
    MenuModeUnknown,
    MenuOutputSize,
    MenuLanguage,
    MenuLanguageAuto,
    MenuRestart,
    MenuAutostart,
    MenuOpenLog,
    MenuInstallDriver,
    MenuExit,
    NoticeSizeTitle,
    NoticeSizeText,
    // Placeholder frames
    PlaceholderNoCamera,
    PlaceholderBusy,
    PlaceholderUnplugged,
    PlaceholderNoSignal,
    PlaceholderAppNotRunning,
    // Install / uninstall messages
    MsgInstallCancelled,
    MsgInstallFailed,
    MsgInstalled,
    MsgUninstallCancelled,
    MsgUninstallPartial,
    MsgUninstalled,
    Count
};

// Switches the language for subsequent tr() calls.
void setRussian(bool russian);
bool isRussian();
const wchar_t *tr(Str id);

}  // namespace wcs
