# WebcamShare

**English** · [Русский](README.ru.md)

Share one webcam with several programs at once on Windows.

WebcamShare takes your real webcam and turns it into a virtual camera,
**"WebcamShare 720p"**, that any number of apps can open at the same time:
for example XR Animator, Warudo and OBS together. Normally a webcam can be used
by only one app at a time.

- One small `.exe` (~700 KB) that lives in the system tray, with no windows.
- Very light: ~0.1 % CPU and ~25 MB RAM with a camera that outputs NV12 natively.
- Recovers on its own when the webcam is unplugged, replugged or busy.
- Virtual camera resolution: 1280×720 (default), 1920×1080 or 640×360, at 30 fps.
- English and Russian interface.

## Requirements

- Windows 10 or 11, 64-bit.
- Administrator rights **once**, to install the virtual camera driver.

## Install

1. Download the latest release and extract it to a folder where it will stay
   (autostart remembers the path).
2. Run **`install.bat`** and accept the administrator prompt.

That's it: the virtual camera is installed, WebcamShare starts in the tray and
will start with Windows from now on.

> **Windows blocks the program?** The exe is not code-signed. SmartScreen can be
> passed with *More info → Run anyway*. **Smart App Control** cannot be bypassed:
> it blocks unsigned code, including the virtual camera inside your apps, so it
> must be turned off for WebcamShare to work.

## Use

In XR Animator, Warudo, OBS or any other app, pick the camera
**"WebcamShare 720p"** (the name stays the same at any resolution).
Don't pick your real webcam there: WebcamShare is using it.

The tray icon shows the state:

| Dot | Meaning |
|---|---|
| 🟢 green | Running, frames are flowing |
| 🟡 yellow | Starting, or the webcam is missing/busy: WebcamShare keeps retrying |
| 🔴 red | Error, or the virtual camera is not installed |

Click the icon for the menu:

| Menu item | What it does |
|---|---|
| *status lines* | e.g. "Running: Razer Kiyo Pro" and the camera mode in use |
| **Camera** | Which real webcam to share (default: the first one found) |
| **Camera mode** | *Automatic* (recommended) or a specific mode of the webcam: format, resolution, 30/60 fps |
| **Virtual camera resolution** | 1920×1080, 1280×720 or 640×360 |
| **Language / Язык** | System default, English or Русский |
| **Restart capture** | Reopens the webcam |
| **Start with Windows** | Turns autostart on or off |
| **Open log** | Opens the log file |
| **Exit** | Closes WebcamShare (apps then see a "not running" picture) |

### Resolution and field of view

- **1280×720** is the best choice for face and body tracking (XR Animator,
  Warudo): trackers downscale the picture anyway, and a smaller frame costs them
  less CPU. Use **1920×1080** when the picture itself matters, e.g. in OBS.
- After changing the resolution, **reopen the camera in your apps**: until then
  they keep receiving the old size (scaled).
- Webcams usually show the full field of view in all 16:9 modes (1080p, 720p,
  360p) and crop the sides in 4:3 modes (640×480). That's why the *Camera mode*
  menu only lists 16:9 modes. If the picture still looks cropped, check the
  webcam's own field-of-view or zoom setting in its vendor software.

### What the virtual camera shows

- Your webcam picture.
- **"WebcamShare: Camera not found / disconnected / busy"**: the webcam is
  unavailable. WebcamShare retries every 3 seconds and right away when a camera
  is plugged in; nothing needs restarting.
- **"WebcamShare is not running"**: the tray program is not running.

## Uninstall

Run **`uninstall.bat`** and accept the administrator prompt. It closes
WebcamShare, turns off autostart and removes the virtual camera driver
(`C:\Program Files\WebcamShare`). Settings and the log stay in
`%APPDATA%\WebcamShare`; delete that folder too if you want.

## Files

| Where | What |
|---|---|
| `%APPDATA%\WebcamShare\settings.json` | Selected camera, camera mode, resolution, language |
| `%APPDATA%\WebcamShare\log.txt` | Log (rotated at 1 MB, two old files kept) |
| `C:\Program Files\WebcamShare\` | Virtual camera driver (32- and 64-bit DirectShow filter) |
| `HKCU\Software\WebcamShare` | Resolution and language, read by the virtual camera inside apps |

## Troubleshooting

- **An app doesn't list "WebcamShare 720p".** The virtual camera is a DirectShow
  device. Desktop apps, browsers, Electron and Unity apps see it; Microsoft Store
  (UWP) apps such as the Windows Camera app don't.
- **"Camera busy".** Another program holds the real webcam. Close it or switch it
  to "WebcamShare 720p"; WebcamShare picks the webcam up by itself.
- **Something else.** *Open log* in the menu: it lists the webcam's modes, the
  mode in use, frame rate statistics and every error.

## How it works

```
webcam ─MF─▶ WebcamShare.exe ─NV12─▶ shared memory ─▶ virtual camera filter in each app ─▶ YUY2/NV12/RGB
```

- **Capture** ([src/app/capture.cpp](src/app/capture.cpp)): Media Foundation
  source reader. In *Automatic* mode it picks the webcam mode closest to the
  output resolution without upscaling, preferring 30 fps and uncompressed
  formats (NV12 → YUY2 → MJPEG); slow uncompressed modes (5–10 fps, common on
  USB 2 webcams) are skipped. MJPEG is decoded by the Media Foundation decoder
  (hardware when available).
- **Frame channel** ([src/common/shared_frame.h](src/common/shared_frame.h)):
  three NV12 slots in shared memory with tear detection, a "frame ready" event,
  one writer only.
- **Virtual camera** ([src/vcam](src/vcam/filter.cpp)): a small DirectShow source
  filter loaded into every app that opens the camera. It reads the shared frames
  and hands them out in the format the app asks for.

## Build from source

Requirements: Visual Studio 2022 Build Tools (C++ workload, Windows SDK) and
CMake 3.20+.

```bash
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

This builds the 32-bit filter (`build\Win32`), then the 64-bit filter, app,
tests and tools (`build\x64`), runs the unit tests and puts `WebcamShare.exe`,
`install.bat`, `uninstall.bat` and the READMEs into `dist\`. Both filter DLLs are
embedded into `WebcamShare.exe`.

Developer tools (in `build\x64\Release`):

- `wcs_tests.exe`: unit tests.
- `vcamprobe.exe`: console DirectShow client. `vcamprobe` lists cameras,
  `vcamprobe WebcamShare 10 YUY2` reads for 10 s and prints the frame rate,
  `vcamprobe <name> controls` prints zoom/pan/focus ranges.
- `patternfeed.exe`: sends a test pattern into the virtual camera (only while
  WebcamShare is not running).
- `camsnap.exe`: saves one frame of a given webcam mode as BMP, to compare
  fields of view.
- `scripts\vcam_register.ps1 -Dev`: registers the filter straight from `build\`
  so rebuilds don't need admin rights.

## License

[MIT](LICENSE)
