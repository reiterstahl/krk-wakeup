$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RecoveryTestWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
}
'@
$folder = Join-Path $env:LOCALAPPDATA 'KRKWakeup'
$settings = Join-Path $folder 'settings.ini'
$log = Join-Path $folder 'diagnostics.log'
$oldLog = Join-Path $folder 'diagnostics.previous.log'
$backups = @{}
foreach ($path in @($settings, $log, $oldLog)) {
    $backups[$path] = if (Test-Path $path) { [System.IO.File]::ReadAllBytes($path) } else { $null }
}
function Read-Log {
    if (Test-Path $log) {
        $stream = [System.IO.File]::Open($log, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
        $reader = [System.IO.StreamReader]::new($stream)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    return ''
}
function Wait-Until([scriptblock] $condition, [string] $description, [int] $timeoutMs = 5000) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $timeoutMs) {
        if (& $condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out: $description`n$(Read-Log)"
}
function Send-AppMessage([uint32] $message, [int] $value) {
    $result = [IntPtr]::Zero
    if ([RecoveryTestWindow]::SendMessageTimeout($script:window, $message, [IntPtr]$value,
            [IntPtr]::Zero, 2, 2000, [ref]$result) -eq [IntPtr]::Zero) {
        throw "App did not handle message $message"
    }
}
$process = $null
try {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Remove-Item $log -ErrorAction SilentlyContinue
    @'
[Settings]
EndpointId=CI-offline-device
IdleSeconds=900
IntervalSeconds=1080
DurationMs=1000
FrequencyHz=80
LevelPercent=1
Enabled=1
Startup=0
'@ | Set-Content -Path $settings -Encoding Unicode
    $process = Start-Process -FilePath 'build/Release/krk-wakeup.exe' -PassThru
    $script:window = [IntPtr]::Zero
    Wait-Until {
        $script:window = [RecoveryTestWindow]::FindWindow('KRKWakeupSettings', 'KRK Wakeup')
        $script:window -ne [IntPtr]::Zero -and (Read-Log) -match 'event=pulse_error'
    } 'initial unavailable-device attempt'
    Send-AppMessage 0x02B1 7 # WM_WTSSESSION_CHANGE / WTS_SESSION_LOCK
    Wait-Until { (Read-Log) -match 'event=session_lock' } 'session lock diagnostic'
    Send-AppMessage 0x0218 4 # WM_POWERBROADCAST / PBT_APMSUSPEND
    Send-AppMessage 0x0218 18 # PBT_APMRESUMEAUTOMATIC
    Wait-Until {
        ([regex]::Matches((Read-Log), 'event=pulse_error\b')).Count -ge 2
    } 'immediate audio retry on resume' 2000
    if ((Read-Log) -notmatch 'event=recovery_requested reason=resume') { throw 'Resume did not rearm the worker' }
    # A failed endpoint must keep retrying while the window is hidden and the
    # session notification says locked, instead of waiting another 18 minutes.
    Wait-Until {
        ([regex]::Matches((Read-Log), 'event=pulse_error\b')).Count -ge 3
    } 'retry while hidden after lock notification' 7000
    Send-AppMessage 0x02B1 8 # WTS_SESSION_UNLOCK
    Send-AppMessage 0x0111 107 # Pause
    $attempts = ([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count
    Send-AppMessage 0x0218 18
    Start-Sleep -Milliseconds 1200
    if (([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count -ne $attempts) {
        throw 'Resume unexpectedly enabled a paused worker'
    }
    Write-Host 'Lock diagnostics, hidden retries, resume recovery, and paused state checks passed.'
} finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    foreach ($path in $backups.Keys) {
        if ($null -eq $backups[$path]) {
            Remove-Item $path -ErrorAction SilentlyContinue
        } else {
            [System.IO.File]::WriteAllBytes($path, $backups[$path])
        }
    }
}
