# VoiceAS

A Windows C++20 utility that automatically selects your preferred Active audio output. Configure a priority order once, then run the monitor in the background. Bluetooth, wired, and USB receiver outputs all follow the same Windows endpoint state and priority rules.

## Build

Requirements: Visual Studio 2026 with the **Desktop development with C++** workload, the v145 toolset, and a Windows SDK. No third-party libraries or administrator privileges are required.

Open `VoiceAS.slnx`, reload the project if Visual Studio detects external changes, select **Release | x64**, and build. You can also use Developer PowerShell from the solution root:

```powershell
msbuild .\VoiceAS.slnx /p:Configuration=Release /p:Platform=x64
```

The executable and launchers are placed in `x64/Release/`. `Debug | x64` uses `x64/Debug/`; building the `.vcxproj` directly may place outputs inside the project directory instead.

When upgrading from an older version without remote stop support, close its monitor console before rebuilding or starting the new version.

## Quick start

Keep these launchers next to `VoiceAS.exe`:

| Launcher | Action |
| --- | --- |
| `Configure.cmd` | Open the interactive priority configuration. |
| `Monitor.cmd` | Start a background monitor and close the launcher window. |
| `StopMonitor.cmd` | Stop the running monitor gracefully. |

1. Run **Configure.cmd**. Only Windows `Active` output endpoints are listed. Enter numbers from highest to lowest priority, such as `2 1 3` or `2,1,3`. You may select a subset. Enter `r` to refresh, or `q`/an empty line to cancel.
2. Choose whether communications audio should follow the same order. The initial default is **No**, keeping voice/call routing independent.
3. Run **Monitor.cmd**. The monitor starts in a separate process without a console window and continues after the launcher exits.
4. Run **StopMonitor.cmd** whenever you want to stop automatic switching.

Running `VoiceAS.exe` without arguments opens a menu with configuration, background start, device listing, and stop options. Closing the menu after starting the background monitor does not stop monitoring.

All built-in messages and logs are in English. Device names retain their original Windows names, including Unicode characters.

## Background control and logs

```powershell
.\VoiceAS.exe --monitor
.\VoiceAS.exe --status
.\VoiceAS.exe --stop
```

Background messages are appended to **`monitor.log` next to the configuration file**. With the default configuration, the log is at `%LOCALAPPDATA%\VoiceAS\monitor.log`.

Startup waits for the worker to initialize its configuration and device notifications before reporting success. Only one monitor may run in a Windows session. Stop it before starting another instance or changing between foreground and background modes.

For live diagnostics in a terminal:

```powershell
# Run in the foreground; Ctrl+C or --stop ends monitoring
.\VoiceAS.exe --monitor --foreground

# Preview decisions without changing audio routing
.\VoiceAS.exe --monitor --foreground --dry-run

# Inspect once, print the result, and exit
.\VoiceAS.exe --monitor --dry-run --once
```

`--monitor --dry-run` without `--foreground` starts a background preview and writes its decisions to the log. `--monitor --once` applies the selected output once and exits; add `--dry-run` to inspect without applying.

## Switching rules

- For an order A, B, C, select the first configured endpoint whose Windows state is Active.
- If A becomes unavailable, fall back to B. If A becomes Active again, switch back to A.
- Disabled, unplugged, and missing endpoints are skipped while their IDs remain in the saved configuration.
- If none of the configured devices are Active, keep the current Windows default and continue waiting. Unconfigured outputs are not selected automatically.
- Console and Multimedia playback roles follow the priority list. Communications follows it only if enabled during configuration.
- Manual changes to the default output are also corrected to match the priority list while monitoring is running. Stop the monitor to choose an output manually.
- Device notifications trigger re-evaluation. A roughly **2-second** check cycle also reloads configuration changes, retries temporary failures, and recovers from missed notifications.
- Saving a new configuration replaces the entire previous priority list. Connect any receivers/devices you want to include before configuring.

VoiceAS changes Windows default outputs. Applications explicitly configured to use a particular device may keep using that device.

Availability follows the Windows endpoint state regardless of connection type. A USB receiver that remains Active is eligible even when its headset is powered off. VoiceAS does not query headset power or battery status or identify the wireless transport. Bluetooth and wired outputs are selected or skipped as Windows reports their state changes.

## Configuration

The default file is **`%LOCALAPPDATA%\VoiceAS\priority.txt`**, shared across build directories and x86/x64 versions. Priorities use endpoint IDs rather than names or temporary list positions. Driver reinstalls may change IDs and require reconfiguration.

The file uses UTF-8 and is replaced as a complete file when saved. Invalid input and cancellation preserve the existing configuration. If a running monitor cannot read a changed configuration, it keeps the last valid version and retries; startup requires a valid configuration.

Use a custom path for a separate configuration and log directory:

```powershell
.\VoiceAS.exe --configure --config "D:\Audio Settings\priority.txt"
.\VoiceAS.exe --monitor --config "D:\Audio Settings\priority.txt"
```

`--stop` and `--status` control the monitor in the current Windows session without requiring its configuration path.

## Full device list and command help

```powershell
.\VoiceAS.exe --list
.\VoiceAS.exe --help
```

The full list includes output endpoint IDs, Windows state, and default roles. It includes speakers, headphones, HDMI, and virtual outputs; microphones are excluded.

| Windows state | Meaning |
| --- | --- |
| `Active` | Present and enabled. |
| `Disabled` | Disabled. |
| `Not present` | Currently missing. |
| `Unplugged` | Not plugged in. |

Successful commands return exit code 0; failures return 1. `--status` returns 0 when a monitor is running and 1 when none is running. Stopping an already stopped monitor succeeds.

## Source layout

| Files | Responsibility |
| --- | --- |
| `EnumerateVoiceDevices.h/.cpp` | Endpoint enumeration, names, Windows states, and default roles. |
| `AudioPriority.h/.cpp` | Priority selection, order input, and configuration persistence. |
| `AudioMonitor.h/.cpp` | Notifications, switching, reloads, and retries. |
| `BackgroundMonitor.h/.cpp` | Detached worker startup, log redirection, startup handshake, and graceful remote stop. |
| `WindowsSupport.h` | COM, handles, UTF-8 conversion, and English error formatting. |
| `VoiceAS.cpp` | English menu, configuration UI, and command-line entry points. |

Enumeration uses [IMMDeviceEnumerator::EnumAudioEndpoints](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-enumaudioendpoints) with `eRender` and `DEVICE_STATEMASK_ALL`. [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient) callbacks only signal the main loop. Background startup uses [CREATE_NO_WINDOW](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags) with explicitly inherited log/input handles.

Default-device changes use the undocumented `IPolicyConfig::SetDefaultEndpoint`, isolated in `AudioMonitor.cpp`; the ABI is referenced from [PolicyConfig declarations](https://github.com/amate/SetDefaultAudioDevice/blob/master/PolicyConfig.h). Recheck compatibility after major Windows changes.

## Tests

Stop any running monitor first, then use Developer PowerShell from the solution root:

```powershell
msbuild .\tests\VoiceAS.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64
.\tests\bin\x64\VoiceAS.Tests.exe

# Exercise real COM setters by reapplying each role to its existing device
.\tests\bin\x64\VoiceAS.Tests.exe --integration

# PowerShell 7: UI, Active filtering, reload/recovery, background lifetime and stop
.\tests\SmokeTests.ps1 -Executable .\x64\Release\VoiceAS.exe
```

Tests cover fallback and reconnection, uniform Active-state selection for Bluetooth, wired, and USB receiver endpoints, duplicate names, unavailable devices, invalid order input, and configuration corruption. Background checks verify that the worker outlives its launcher, has no console window, writes logs, rejects duplicate instances, and stops gracefully. Automated tests simulate endpoint state changes; they do not physically connect or disconnect devices.

To sample an existing listener without stopping it:

```powershell
.\tests\MeasureMonitor.ps1 -ExistingProcessId <PID> -Seconds 30
```
