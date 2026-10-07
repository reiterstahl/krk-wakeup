$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeWindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
}
'@
function Save-AppWindow([IntPtr] $window, [string] $path) {
    $rect = New-Object NativeWindowCapture+Rect
    if (-not [NativeWindowCapture]::GetWindowRect($window, [ref] $rect)) { throw 'GetWindowRect failed' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 300 -or $height -lt 300) { throw "Unexpected window size: $width x $height" }
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try {
            if (-not [NativeWindowCapture]::PrintWindow($window, $dc, 0)) { throw 'PrintWindow failed' }
        } finally { $graphics.ReleaseHdc($dc) }
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}
$process = Start-Process -FilePath 'build/Release/krk-wakeup.exe' -PassThru
try {
    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        Start-Sleep -Milliseconds 500
        $window = [NativeWindowCapture]::FindWindow('KRKWakeupSettings', 'KRK Wakeup')
        if ($window -ne [IntPtr]::Zero) { break }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'App window was not found' }
    [NativeWindowCapture]::SetForegroundWindow($window) | Out-Null
    Start-Sleep -Seconds 2
    New-Item -ItemType Directory -Force -Path 'capture' | Out-Null
    Save-AppWindow $window 'capture/ui-compact.png'
    [NativeWindowCapture]::SendMessage($window, 0x0111, [IntPtr]112, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    Save-AppWindow $window 'capture/ui-expanded.png'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
