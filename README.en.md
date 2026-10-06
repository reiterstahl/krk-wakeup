<p align="center">
  <img src="assets/app-icon.png" alt="KRK Wakeup app icon" width="96" height="96">
</p>

<p align="center">
  <a href="README.md">Español</a> · <a href="README.en.md">English</a>
</p>

<p align="center">
  <img alt="Windows 11 x64" src="https://img.shields.io/badge/Windows_11-x64-0078D4?style=flat-square&logo=windows11&logoColor=white">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white">
  <img alt="Win32" src="https://img.shields.io/badge/UI-Win32-202526?style=flat-square&logo=windows11&logoColor=white">
  <img alt="WASAPI" src="https://img.shields.io/badge/Audio-WASAPI-202526?style=flat-square&logo=windows11&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/Build-CMake-064F8C?style=flat-square&logo=cmake&logoColor=white">
  <img alt="GitHub Actions" src="https://img.shields.io/badge/CI-GitHub_Actions-2088FF?style=flat-square&logo=githubactions&logoColor=white">
</p>

**KRK Wakeup** is a small Windows utility that sends a short audio signal to an output you choose. It was created to prevent KRK GoAux 3 speakers connected over RCA to an SMSL SU-1 from entering standby during long pauses.

> **Status:** working prototype. The Windows workflow verifies that the app builds and starts. The pulse's effectiveness and audibility are still being tested on the physical speakers.

<p align="center">
  <img src="assets/banner.svg" alt="KRK Wakeup: a short signal to keep your speakers ready" width="100%">
</p>

<p align="center">
  <img src="assets/ui-compact.png" alt="Compact KRK Wakeup window with a dark theme" width="420">
</p>

## Why it exists

GoAux speakers can enter standby after receiving no signal for some time. In this setup, the initial estimate is **15 minutes**. The [official KRK manual](https://cdn.shopify.com/s/files/1/0566/0809/6342/files/KRK-GoAux-Monitor-System-Product-User-Manual.pdf?v=1705178896) explains how to control standby manually, but does not specify the automatic standby interval or the minimum signal level that resets it. Both need to be checked with the actual speakers.

The idea is simple: while the utility is active, it sends a discreet pulse before the speakers go to sleep. The default test interval is **5 minutes**, and you can change it.

## Features

- A notification area icon next to the Windows clock and a compact OLED black window.
- A fixed audio output: changing the Windows default output to Bluetooth headphones does not change where the pulse goes.
- A three-second audible test tone (440 Hz at 5% digital level), a test of the configured pulse, pause, an adjustable interval, and a countdown to the next pulse.
- Additional settings for estimated standby time, pulse duration, frequency, and digital level.
- Optional startup with Windows for the current user.
- Wait and retry when the selected output disappears; pulses are never redirected automatically to another output.

The initial target setup is **Windows 11 x64 → USB → SMSL SU-1 → RCA → KRK GoAux 3**. Windows 10 is a later compatibility goal and still requires testing.

## Download and test

1. Open the latest successful [Windows build](https://github.com/reiterstahl/krk-wakeup/actions/workflows/windows.yml) and download the **`krk-wakeup-windows-x64`** artifact. Extract `krk-wakeup.exe` to a stable folder.
2. If an older version is running, choose **Salir** (Exit) from its notification area menu before replacing the `.exe`. Your settings will remain in place.
3. Open the app and select the playback output corresponding to the **SMSL SU-1**. You can compare its name with **Settings → System → Sound** in Windows.
4. Set the GoAux speakers to a comfortable volume, then click **Probar tono** (Test tone). It plays for three seconds at 440 Hz and 5% digital level only through the selected output, even if the app is paused. Confirm that you hear it on the GoAux and nowhere else. Next, click **Probar pulso** (Test pulse): it uses the configured automatic signal, initially **440 Hz**, **1000 ms**, and **1%** digital level. Change these values under **Más ajustes** (More settings) if the signal is annoying or fails to prevent standby.
5. Save your settings. Switch the Windows default output to Bluetooth headphones and confirm the test still plays only through the DAC. Then leave the GoAux without other audio for longer than their usual standby time and check whether they remain active.

Time fields use `mm:ss`. **Reposo estimado** (Estimated standby) is a calibration reference; it does not change the KRK speakers' internal timer. **Intervalo entre pulsos** (Pulse interval) controls when the app sends audio. The app warns you if the interval equals or exceeds the estimated standby time.

**Frecuencia** (Frequency) accepts **10 to 15000 Hz**. To try a low frequency, use **Probar pulso** (Test pulse), which plays your configured settings. **Probar tono** (Test tone) remains the fixed audible test at 440 Hz. Low frequencies are experimental: their effectiveness at preventing standby still needs to be checked on the GoAux.

If the GoAux still enter standby despite a five-minute pulse interval, first use **Probar tono** to check the audio path. Then try a **3%** digital pulse level and **2000 ms** duration while keeping the five-minute interval. If they still go to sleep, try **5%** and observe at least two full standby periods. Lower the level if it becomes annoying. KRK does not publish the internal detection threshold in its manual, so these settings are test suggestions, not a guarantee. Also check that neither Windows nor the SMSL SU-1 is muted.

**Locking Windows with Win + L** leaves the utility running while the computer remains awake. If Windows enters sleep or hibernation, desktop processes pause and pulses resume when the computer wakes. See [Microsoft's sleep documentation](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/integrating-apps-with-modern-standby).

## How it works

The app uses the [Windows audio device API](https://learn.microsoft.com/en-us/windows/win32/coreaudio/getting-the-default-device-endpoint-for-stream-routing) to save the selected output's identifier and open that output directly. It generates a short waveform with smooth fade-in and fade-out through [WASAPI shared mode](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi). Regular audio and Bluetooth headphones can coexist with the utility.

With this analog connection, Windows identifies the **SMSL SU-1**, not the speakers attached to its RCA outputs. The app can detect when the USB DAC disappears, but it cannot detect an unplugged RCA cable or powered-off GoAux speakers. Nor can software confirm that a pulse reset the speakers' internal standby timer: that must be checked on the speakers themselves.

The window and notification area icon use Win32; audio uses WASAPI; builds use CMake and MSVC. The executable links the C++ runtime statically. Settings are stored in `%LOCALAPPDATA%\KRKWakeup\settings.ini`; optional startup uses `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The app needs no administrator privileges and does not modify the speakers' firmware.

## Build

Install Visual Studio 2022 with **Desktop development with C++** and CMake. In PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be at `build\Release\krk-wakeup.exe`. Every change to `main` triggers a build and startup smoke test in GitHub Actions. This test does not measure physical standby behavior; the [validation plan](docs/PLAN.md) describes that check (currently in Spanish).

## Project

This repository is public so the prototype and its tests can be shared. An open-source license has not yet been chosen; until then, publishing the code grants no additional rights to use or redistribute it. KRK Wakeup is an independent project and is not affiliated with KRK, Gibson, or SMSL.
