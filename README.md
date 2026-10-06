# KRK Wakeup

Small native Windows utility that periodically sends an audio pulse to a selected playback device. The initial setup is Windows 11 x64 → USB → SMSL SU-1 → RCA → KRK GoAux 3. Switching the default Windows output to Bluetooth headphones does not change the selected pulse destination.

**Status:** first prototype. A successful audio write does not prove that the speakers stayed awake. The pulse level and interval need testing on the real GoAux 3. This is an independent project, not an official KRK product.

## Get the executable

The private repository's [Windows build workflow](.github/workflows/windows.yml) creates a `krk-wakeup-windows-x64` artifact containing `krk-wakeup.exe`. Download the artifact from a successful GitHub Actions run and extract the ZIP. The program is portable; keep the executable in a stable location if you enable startup with Windows.

To compile locally, install Visual Studio 2022 with the **Desktop development with C++** workload and CMake, then run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be at `build\Release\krk-wakeup.exe`.

## First test

1. Connect the SMSL SU-1 via USB and the GoAux 3 via RCA. Start the app; it opens the settings window on first launch and adds an icon next to the Windows clock. Windows may hide the icon behind the `^` menu.
2. In **Salida de audio para los KRK**, choose the Windows playback device corresponding to the SMSL SU-1. Do not select the Bluetooth headphones. If the name is unclear, use Windows **Settings → System → Sound** to identify the DAC.
3. Start with the default **15:00** estimated standby time, **05:00** pulse interval, **1000 ms** duration, **440 Hz** frequency, and **1 %** digital level. Keep the speakers at a comfortable volume and select **Probar señal**. The level, frequency, and duration are experimental.
4. Save the settings. The app remains in the notification area when its window is closed. Its menu can pause it, test one pulse, reopen settings, or exit.
5. Set Bluetooth headphones as the Windows default output. Verify that the test pulse still uses the DAC. Then leave the speakers idle for longer than their usual standby period and observe whether they stay awake. Adjust the interval and signal as needed.

The time fields use `mm:ss`. The estimated standby time is a reference and does not change the speakers' hardware timer. The selected DAC is stored by Windows endpoint ID. If the DAC disappears, the app waits and retries; it never switches to another output automatically. Windows cannot detect RCA disconnection or speaker power state while the DAC remains connected.

Settings are stored in `%LOCALAPPDATA%\KRKWakeup\settings.ini`. Optional startup uses the current user's Windows Run setting. No administrator access is required. To uninstall, disable startup in the app, exit, remove the executable, and optionally remove the settings directory.

## Project notes

See [the plan](docs/PLAN.md) for scope, research, and physical validation. Windows 10 compatibility remains a later goal. The repository is private with a future public open-source release intended. A license has not yet been selected.
