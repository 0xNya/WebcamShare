# Builds WebcamShare (x86: filter only; x64: filter, app with both filters embedded, tools)
# into build\<arch>. Win32 goes first: the x64 app embeds its DLL.
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Config Release] [-Arch Win32,x64]
param(
    [string]$Config = 'Release',
    [string[]]$Arch = @('Win32', 'x64'),
    [string]$CMake = ''
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

if (-not $CMake) {
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { $CMake = $c.Source }
    else {
        $candidates = @(
            "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Kitware.CMake_Microsoft.Winget.Source_8wekyb3d8bbwe\cmake-*\bin\cmake.exe",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
        $CMake = $candidates | ForEach-Object { Get-ChildItem $_ -ErrorAction SilentlyContinue } |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $CMake) { throw 'cmake not found' }
    }
}

foreach ($a in $Arch) {
    $dir = Join-Path $root "build\$a"

    # A dev-registered filter DLL may be loaded by camera clients: a loaded
    # DLL can't be overwritten but can be renamed out of the linker's way.
    $dll = Join-Path $dir "$Config\WebcamShareCam.dll"
    Get-ChildItem (Join-Path $dir "$Config\*.dll.old") -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    if (Test-Path $dll) {
        try { [IO.File]::Open($dll, 'Open', 'ReadWrite', 'None').Close() }
        catch { Rename-Item $dll "WebcamShareCam.$(Get-Date -Format yyyyMMddHHmmss).dll.old" }
    }

    $x86Dll = Join-Path $root "build\Win32\$Config\WebcamShareCam.dll"
    & $CMake -S $root -B $dir -G 'Visual Studio 17 2022' -A $a "-DWCS_X86_DLL=$x86Dll"
    if ($LASTEXITCODE) { throw "configure $a failed" }
    & $CMake --build $dir --config $Config --parallel
    if ($LASTEXITCODE) { throw "build $a failed" }
}
