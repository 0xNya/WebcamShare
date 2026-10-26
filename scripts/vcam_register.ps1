# Development helper: registers the WebcamShare filter DLLs from build\ (admin,
# self-elevating). Users install with "WebcamShare.exe --install" instead.
#   powershell -ExecutionPolicy Bypass -File scripts\vcam_register.ps1 [-Unregister] [-Dev]
# -Dev registers the DLLs in place from build\, so rebuilds need no admin.
param(
    [switch]$Unregister,
    [switch]$Dev,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Config', $Config)
    if ($Unregister) { $argList += '-Unregister' }
    if ($Dev) { $argList += '-Dev' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -PassThru
    $p.WaitForExit()
    Get-Content (Join-Path $env:TEMP 'vcam_register.log')
    exit $p.ExitCode
}

$log = Join-Path $env:TEMP 'vcam_register.log'
Start-Transcript -Path $log -Force | Out-Null
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dst = Join-Path $env:ProgramFiles 'WebcamShare'
$targets = @(
    @{ Arch = 'x64'; Src = "build\x64\$Config"; RegSvr = "$env:windir\System32\regsvr32.exe" },
    @{ Arch = 'x86'; Src = "build\Win32\$Config"; RegSvr = "$env:windir\SysWOW64\regsvr32.exe" })

function Remove-Locked([string]$path) {
    # Files loaded by running processes cannot be deleted but can be renamed,
    # then removed at the next reboot.
    try { Remove-Item -Force $path -ErrorAction Stop }
    catch {
        $old = "$path.$(Get-Date -Format yyyyMMddHHmmss).old"
        Rename-Item $path $old
        Add-Type -Namespace Win32 -Name Native -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool MoveFileEx(string a, string b, int f);'
        [Win32.Native]::MoveFileEx($old, $null, 4) | Out-Null  # MOVEFILE_DELAY_UNTIL_REBOOT
        "scheduled for deletion at reboot: $old"
    }
}

try {
    foreach ($t in $targets) {
        $dll = Join-Path $dst "$($t.Arch)\WebcamShareCam.dll"
        $devDll = Join-Path $root "$($t.Src)\WebcamShareCam.dll"
        foreach ($old in @($dll, $devDll)) {
            if (Test-Path $old) {
                & $t.RegSvr /s /u $old
                "unregistered $($t.Arch): $old ($LASTEXITCODE)"
            }
        }
        if ($Dev -and -not $Unregister) {
            & $t.RegSvr /s $devDll
            if ($LASTEXITCODE) { throw "regsvr32 $($t.Arch) failed: $LASTEXITCODE" }
            "registered $($t.Arch) in place: $devDll"
            continue
        }
        if ($Unregister) {
            if (Test-Path $dll) { Remove-Locked $dll }
            continue
        }
        New-Item -ItemType Directory -Force (Split-Path $dll) | Out-Null
        if (Test-Path $dll) { Remove-Locked $dll }
        Copy-Item (Join-Path $root "$($t.Src)\WebcamShareCam.dll") $dll
        & $t.RegSvr /s $dll
        if ($LASTEXITCODE) { throw "regsvr32 $($t.Arch) failed: $LASTEXITCODE" }
        "registered $($t.Arch): $dll"
    }
} catch {
    $_ | Out-String
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
