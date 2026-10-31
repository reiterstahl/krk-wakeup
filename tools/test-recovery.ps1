$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RecoveryTestWindow {
    [StructLayout(LayoutKind.Sequential)] public struct DisplaySetting {
        public Guid Setting; public uint Length; public uint State;
    }
    public static IntPtr AllocateDisplay(uint state) {
        var value = new DisplaySetting { Setting = new Guid("2B84C20E-AD23-4DDF-93DB-05FFBD7EFCA5"), Length = 4, State = state };
        IntPtr pointer = Marshal.AllocHGlobal(Marshal.SizeOf<DisplaySetting>());
        Marshal.StructureToPtr(value, pointer, false);
        return pointer;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
}
'@
$folder = Join-Path $env:LOCALAPPDATA 'KRKWakeup'
$settings = Join-Path $folder 'settings.ini'
$logs = Join-Path $folder 'Logs'
$previousSettings = if (Test-Path $settings) { [System.IO.File]::ReadAllBytes($settings) } else { $null }
function Current-Log {
    if (-not $process -or -not (Test-Path $logs)) { return $null }
    return Get-ChildItem -Path $logs -Filter 'krk-wakeup-*.log' |
        Where-Object { $_.Name -match "-$($process.Id)(?:-part\d+)?\.log$" } |
        Sort-Object LastWriteTime | Select-Object -Last 1
}
function Read-Log {
    $file = Current-Log
    if (-not $file) { return '' }
    $stream = [System.IO.File]::Open($file.FullName, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    $reader = [System.IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Wait-Until([scriptblock] $condition, [string] $description, [int] $timeoutMs = 5000) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $timeoutMs) {
        if (& $condition) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out: $description`n$(Read-Log)"
}
function Send-AppMessage([uint32] $message, [int] $value, [IntPtr] $data = [IntPtr]::Zero) {
    $result = [IntPtr]::Zero
    if ([RecoveryTestWindow]::SendMessageTimeout($script:window, $message, [IntPtr]$value,
            $data, 2, 2000, [ref]$result) -eq [IntPtr]::Zero) {
        throw "App did not handle message $message"
    }
}
function Send-DisplayState([uint32] $state) {
    $pointer = [RecoveryTestWindow]::AllocateDisplay($state)
    try { Send-AppMessage 0x0218 0x8013 $pointer }
    finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($pointer) }
}
$process = $null
try {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
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
Diagnostics=1
'@ | Set-Content -Path $settings -Encoding Unicode
    $process = Start-Process -FilePath 'build/Release/krk-wakeup.exe' -PassThru
    $script:window = [IntPtr]::Zero
    Wait-Until {
        $script:window = [RecoveryTestWindow]::FindWindow('KRKWakeupSettings', 'KRK Wakeup')
        $script:window -ne [IntPtr]::Zero -and (Read-Log) -match 'event=pulse_error'
    } 'initial unavailable-device attempt'
    foreach ($state in @(0, 2, 1, 0)) {
        Send-DisplayState $state
        $effective = if ($state -eq 0) { 300 } else { 1080 }
        Wait-Until { (Read-Log) -match "event=screen_policy display_state=$state normal_interval_s=1080 effective_interval_s=$effective" } 'screen interval policy'
    }
    Send-AppMessage 0x02B1 7 # WM_WTSSESSION_CHANGE / WTS_SESSION_LOCK
    Wait-Until { (Read-Log) -match 'event=session_lock' } 'session lock diagnostic'
    $startsBeforeResume = ([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count
    Send-AppMessage 0x0218 4 # WM_POWERBROADCAST / PBT_APMSUSPEND
    Send-AppMessage 0x0218 18 # PBT_APMRESUMEAUTOMATIC
    Wait-Until {
        ([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count -gt $startsBeforeResume
    } 'immediate audio retry on resume' 4000
    if ((Read-Log) -notmatch 'event=recovery_requested reason=resume') { throw 'Resume did not rearm the worker' }
    # A failed endpoint must keep retrying while the window is hidden and the
    # session notification says locked, instead of waiting another 18 minutes.
    $errorsBeforeRetry = ([regex]::Matches((Read-Log), 'event=pulse_error\b')).Count
    Wait-Until {
        ([regex]::Matches((Read-Log), 'event=pulse_error\b')).Count -gt $errorsBeforeRetry
    } 'retry while hidden after lock notification' 7000
    if ((Read-Log) -notmatch 'event=endpoint_observation state=unavailable') {
        throw 'Missing selected-endpoint monitoring diagnostic'
    }
    Send-AppMessage 0x0111 207 # Turn optional diagnostics off
    $offSize = (Current-Log).Length
    Start-Sleep -Milliseconds 5500
    if ((Current-Log).Length -ne $offSize) { throw 'Disabled diagnostics kept writing' }
    Send-AppMessage 0x0111 207 # Enable again without saving unrelated audio settings
    Wait-Until { (Read-Log) -match 'event=instance_snapshot' } 'diagnostics session snapshot'
    Send-AppMessage 0x02B1 8 # WTS_SESSION_UNLOCK
    Send-AppMessage 0x0111 107 # Pause
    $attempts = ([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count
    Send-AppMessage 0x0218 18
    Start-Sleep -Milliseconds 1200
    if (([regex]::Matches((Read-Log), 'event=pulse_start\b')).Count -ne $attempts) {
        throw 'Resume unexpectedly enabled a paused worker'
    }
    $manualAttempts = ([regex]::Matches((Read-Log), 'event=pulse_start kind=test\b')).Count
    Send-AppMessage 0x0111 203 # Manual pulse still works while paused/screen-off
    Wait-Until { ([regex]::Matches((Read-Log), 'event=pulse_start kind=test\b')).Count -gt $manualAttempts } 'manual pulse bypass'
    Write-Host 'Screen interval policy, lock diagnostics, hidden retries, resume recovery, pause, and manual pulse checks passed.'
} finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($null -eq $previousSettings) {
        Remove-Item $settings -ErrorAction SilentlyContinue
    } else {
        [System.IO.File]::WriteAllBytes($settings, $previousSettings)
    }
    if ($process -and (Test-Path $logs)) {
        Get-ChildItem -Path $logs -Filter 'krk-wakeup-*.log' |
        Where-Object { $_.Name -match "-$($process.Id)(?:-part\d+)?\.log$" } | Remove-Item
    }
}
