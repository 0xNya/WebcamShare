# Builds everything and assembles the deliverable into dist\:
#   WebcamShare.exe (filters embedded), install.bat, uninstall.bat, README.md
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dist = Join-Path $root 'dist'

& (Join-Path $PSScriptRoot 'build.ps1')
if ($LASTEXITCODE) { throw 'build failed' }

& (Join-Path $root 'build\x64\Release\wcs_tests.exe')
if ($LASTEXITCODE) { throw 'tests failed' }

New-Item -ItemType Directory -Force $dist | Out-Null
$exe = Join-Path $dist 'WebcamShare.exe'

# The installed copy may be running from dist: move it aside (allowed while running).
if (Test-Path $exe) {
    Get-ChildItem (Join-Path $dist '*.old') -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    try { Remove-Item -Force $exe } catch { Rename-Item $exe "WebcamShare.$(Get-Date -Format yyyyMMddHHmmss).exe.old" }
}

Copy-Item (Join-Path $root 'build\x64\Release\WebcamShare.exe') $exe
Copy-Item (Join-Path $root 'packaging\install.bat') $dist -Force
Copy-Item (Join-Path $root 'packaging\uninstall.bat') $dist -Force
Copy-Item (Join-Path $root 'README.md') $dist -Force
Copy-Item (Join-Path $root 'README.ru.md') $dist -Force
Copy-Item (Join-Path $root 'LICENSE') $dist -Force
Get-ChildItem $dist | Select-Object Name, Length
