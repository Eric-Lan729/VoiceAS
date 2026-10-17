# VoiceAS

A Windows C++20 utility that automatically selects your preferred available audio output. Configure a priority order once, then run the monitor in the background. ROG Delta II support also checks whether the headset itself is connected, even when its USB receiver keeps the Windows audio endpoint Active.

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

- For an order A, B, C, select the first configured endpoint that is Active and ready. A supported ROG wireless headset must additionally be Online.
- If A becomes unavailable, fall back to B. If A becomes ready again, switch back to A.
- Disabled, unplugged, and missing endpoints are skipped while their IDs remain in the saved configuration.
- If none of the configured devices are ready, keep the current Windows default and continue waiting. Unconfigured outputs are not selected automatically.
- Console and Multimedia playback roles follow the priority list. Communications follows it only if enabled during configuration.
- Manual changes to the default output are also corrected to match the priority list while monitoring is running. Stop the monitor to choose an output manually.
- Device notifications trigger re-evaluation. A roughly **2-second** check cycle also updates wireless readiness, reloads configuration changes, and retries temporary failures. Query timeouts can add latency.
- Saving a new configuration replaces the entire previous priority list. Connect any receivers/devices you want to include before configuring.

VoiceAS changes Windows default outputs. Applications explicitly configured to use a particular device may keep using that device.

## ROG Delta II power/readiness detection

Windows `Active` indicates that an audio endpoint is usable; a 2.4 GHz receiver may remain Active while its headset is powered off. VoiceAS queries the ROG Delta II receiver's HID interface (**VID `0B05`, PID `1AFA`**) and matches it to the audio endpoint through the Windows Container ID.

| Headset status | Behavior |
| --- | --- |
| `Online` | A valid readiness/battery response was received; the headset may be selected. |
| `Offline` | The receiver explicitly reports that the headset is not ready; select the next available configured output. |
| `Unknown` | Access failed, the query timed out, or the response was invalid; log the reason and skip the headset for now. |

Switching the headset off or losing its wireless connection triggers fallback. Turning it on and reconnecting to the receiver restores its configured priority. Put the headset before your speakers and include at least one fallback output. If the headset is your only configured output, the usual "keep the current Windows default" rule still applies when it is unavailable.

Configuration continues to list Active Windows endpoints, with an extra headset status label. A powered-off headset can therefore be added to the order while its receiver remains connected.

Inspect headset readiness without changing defaults:

```powershell
.\VoiceAS.exe --headset-status
```

Only the battery/readiness GET query is sent; sound settings, RGB, sleep settings, and firmware are not modified. Other headset models use standard Windows endpoint state detection.

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

The full list includes output endpoint IDs, state, default roles, and supported wireless headset readiness. It includes speakers, headphones, HDMI, and virtual outputs; microphones are excluded.

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
| `EnumerateVoiceDevices.h/.cpp` | Endpoint enumeration, properties, default roles, and Container IDs. |
| `AudioPriority.h/.cpp` | Priority selection, order input, and configuration persistence. |
| `AudioMonitor.h/.cpp` | Notifications, switching, readiness checks, reloads, and retries. |
| `BackgroundMonitor.h/.cpp` | Detached worker startup, log redirection, startup handshake, and graceful remote stop. |
| `WirelessHeadset.h/.cpp` | ROG HID queries, response validation, and endpoint matching. |
| `WindowsSupport.h` | COM, handles, UTF-8 conversion, and English error formatting. |
| `VoiceAS.cpp` | English menu, configuration UI, and command-line entry points. |

Enumeration uses [IMMDeviceEnumerator::EnumAudioEndpoints](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-enumaudioendpoints) with `eRender` and `DEVICE_STATEMASK_ALL`. [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient) callbacks only signal the main loop. Background startup uses [CREATE_NO_WINDOW](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags) with explicitly inherited log/input handles.

Default-device changes use the undocumented `IPolicyConfig::SetDefaultEndpoint`, isolated in `AudioMonitor.cpp`; the ABI is referenced from [PolicyConfig declarations](https://github.com/amate/SetDefaultAudioDevice/blob/master/PolicyConfig.h). Recheck compatibility after major Windows changes.

ROG command and response formats were checked against [G-Helper's headset implementation](https://github.com/seerge/g-helper/blob/main/app/Peripherals/Headset/AsusHeadset.cs). Physical off/on checks on a ROG Delta II confirmed an unavailable response (`CC 12 07 00 00 FF AA ...`) while off and a valid battery response while connected.

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

Tests cover fallback and reconnection, duplicate names, unavailable devices, invalid order input, configuration corruption, recorded ROG power-state replies, malformed HID packets, and query failures. Background checks verify that the worker outlives its launcher, has no console window, writes logs, rejects duplicate instances, and stops gracefully. Automated tests do not control the physical headset power switch.

To sample an existing listener without stopping it:

```powershell
.\tests\MeasureMonitor.ps1 -ExistingProcessId <PID> -Seconds 30
```
