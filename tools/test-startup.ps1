$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StartupTestWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@
$exe = (Resolve-Path 'build/Release/krk-wakeup.exe').Path
$expected = '"' + $exe + '"'
$settings = Join-Path $env:LOCALAPPDATA 'KRKWakeup/settings.ini'
$settingsDirectory = Split-Path $settings
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$previousSettings = if (Test-Path $settings) { [System.IO.File]::ReadAllBytes($settings) } else { $null }
$previousRun = (Get-ItemProperty -Path $runKey -Name KRKWakeup -ErrorAction SilentlyContinue).KRKWakeup
$process = $null
try {
    New-Item -ItemType Directory -Force -Path $settingsDirectory | Out-Null
    New-Item -Path $runKey -Force | Out-Null
    @'
[Settings]
EndpointId=CI-offline-device
IdleSeconds=900
IntervalSeconds=300
DurationMs=1000
FrequencyHz=440
LevelPercent=1
Enabled=0
Startup=1
'@ | Set-Content -Path $settings -Encoding Unicode
    New-ItemProperty -Path $runKey -Name KRKWakeup -PropertyType String -Value '"C:\missing\krk-wakeup.exe"' -Force | Out-Null
    $process = Start-Process -FilePath $exe -PassThru
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        Start-Sleep -Milliseconds 500
        $process.Refresh()
        if ($process.HasExited) { throw "App exited during startup: $($process.ExitCode)" }
        $window = [StartupTestWindow]::FindWindow('KRKWakeupSettings', 'KRK Wakeup')
        if ($window -ne [IntPtr]::Zero) { break }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'App window was not found' }
    $registered = (Get-ItemPropertyValue -Path $runKey -Name KRKWakeup)
    if ($registered -ine $expected) { throw "Launch did not repair Run entry: $registered" }
    New-ItemProperty -Path $runKey -Name KRKWakeup -PropertyType String -Value '"C:\missing\krk-wakeup.exe"' -Force | Out-Null
    [StartupTestWindow]::SendMessage($window, 0x0111, [IntPtr]109, [IntPtr]::Zero) | Out-Null
    $registered = (Get-ItemPropertyValue -Path $runKey -Name KRKWakeup)
    if ($registered -ine $expected) { throw "Save did not repair Run entry: $registered" }
    Write-Host 'Startup registration repaired on launch and save.'
} finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
    if ($null -eq $previousSettings) {
        Remove-Item -Path $settings -ErrorAction SilentlyContinue
    } else {
        [System.IO.File]::WriteAllBytes($settings, $previousSettings)
    }
    if ($null -eq $previousRun) {
        Remove-ItemProperty -Path $runKey -Name KRKWakeup -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $runKey -Name KRKWakeup -PropertyType String -Value $previousRun -Force | Out-Null
    }
}
