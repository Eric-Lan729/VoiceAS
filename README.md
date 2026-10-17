# VoiceAS

Windows C++20 控制台程序：配置输出设备的优先顺序，并持续监听、自动切换到最高优先级的可用设备。

## 运行

使用 Visual Studio 2026 打开 `VoiceAS.slnx`，选择 `Debug | x64`，按 **Ctrl+F5** 编译运行。
如果 Visual Studio 提示项目文件被外部修改，请选择重新加载项目，使新增源文件出现在解决方案资源管理器中。
需要安装“使用 C++ 的桌面开发”工作负载和 Windows SDK，无需第三方库或管理员权限。

也可以在 Visual Studio Developer PowerShell 中执行：

```powershell
msbuild .\VoiceAS.slnx /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\VoiceAS.exe
```

无参数运行会显示菜单：

1. **配置设备优先顺序**：只显示当前 `Active` 的输出设备，按序号从高到低输入，例如 `2 1 3` 或 `2,1,3`。ROG Delta II 额外标注耳机本体的 `Online` / `Offline` / `Unknown` 状态；即使关机，只要接收器仍插着且端点 Active，也可以加入优先列表。可只选部分设备；`r` 刷新列表，`q` 或空行取消。可以选择是否同时切换通信设备，首次配置默认为否。
2. **开始监听并自动切换**：读取保存的配置，启动时立即应用，再持续监听设备变化。窗口可以最小化，按 **Ctrl+C** 停止监听并返回菜单。
3. **列出所有输出设备**：保留原有的全部设备列表功能，含设备 ID、状态和默认角色标记。

构建后，程序所在目录会生成两个可双击的入口：

- `Configure.cmd`：直接进入配置。
- `Monitor.cmd`：直接开始监听，按 **Ctrl+C** 停止。

启动脚本必须与 `VoiceAS.exe` 放在同一目录。通过 solution 构建的 `Debug | x64` 输出目录为 `x64/Debug/`；如果直接构建 `.vcxproj`，默认输出目录位于项目子目录内。

## 自动切换规则

- 保存顺序为 A、B、C 时，只在这些设备中选择第一个 `Active` 且可用的输出端点。ROG Delta II 还必须确认无线链路为 `Online`。
- A 不可用时选择 B；A 重新变为 `Active` 后自动切回 A。
- 已禁用、未插入、当前不存在的设备不参与本次选择；配置仍保留其 ID，重连后可重新参与。
- 配置中的设备都不可用时，保留 Windows 当前选择并继续等待，不主动切到未配置的设备。
- 默认切换 Console 和 Multimedia 两种播放角色；通信角色可在配置时选择跟随或独立。
- 监听期间手动改默认设备，也会重新按优先顺序应用；需要临时手动选择时，请先停止监听。
- 设备通知在主线程触发重新评估，合并短时间内的重复通知；另有约 2 秒的检查周期用于配置更新和临时失败重试。
- 修改配置并保存后，正在运行的监听进程会自动加载。只允许同一 Windows 会话内运行一个监听进程。
- 重新配置会替换整个优先列表。要加入当前离线的设备，请先连接它，再进入配置入口。

这里修改的是 Windows 默认输出设备；显式指定其他输出设备的应用可能继续使用自己的选择。

## ROG Delta II 开关机识别

Windows 的 `Active` 表示音频端点可用，不能单独说明 2.4GHz 无线耳机本体是否开机。USB 接收器仍插着时，关机的耳机也可能保持 Active。
程序针对 ROG Delta II 接收器（VID `0B05`、PID `1AFA`）读取 HID 就绪状态，通过 Windows Container ID 将控制接口与音频端点匹配，不依赖设备名称。

- `Online`：收到有效的就绪/电池回复，耳机可以参与优先级选择。
- `Offline`：接收器明确返回耳机不可就绪，监听跳过耳机并选择下一台可用设备。
- `Unknown`：接口无法访问、查询超时或回复无效，显示诊断信息并暂不选择耳机，不会把查询错误说成关机。

耳机关机或无线链路断开后会降级到下一台设备；开机并重新连接接收器后会按原优先级切回。
无线就绪状态约每 2 秒查询一次，临时通信超时会额外增加延迟。保留现有优先列表即可，建议将耳机排在扬声器前面，并包含至少一个备用设备。
如果配置中只有耳机，耳机不可用时仍遵循“保留 Windows 当前选择”的规则。

可单独检查当前状态，不切换设备：

```powershell
.\VoiceAS.exe --headset-status
```

这项检测只发送电池/就绪状态 GET 查询，不改变音效、RGB、休眠或其他设置；其他耳机型号继续使用 Windows 端点状态。

## 配置保存

默认配置文件为 `%LOCALAPPDATA%\VoiceAS\priority.txt`，不同构建目录与 x86/x64 版本共用同一份配置。
配置按 endpoint ID 保存，不依赖设备名称或显示序号。驱动重装等操作可能改变 ID，此时需要重新配置。
配置文件采用 UTF-8，保存时整体替换；取消配置或输入无效不会覆盖原文件。
若监听期间文件被损坏或暂时无法读取，会保留最近一次有效配置、提示错误，并继续尝试读取；首次启动缺少有效配置则提示先配置。

## 命令行

```powershell
.\VoiceAS.exe --configure
.\VoiceAS.exe --monitor
.\VoiceAS.exe --list
.\VoiceAS.exe --headset-status

# 预览目标设备，不修改当前默认设备
.\VoiceAS.exe --monitor --dry-run

# 只检查一次，适合诊断
.\VoiceAS.exe --monitor --dry-run --once

# 所有入口都可以指定自己的配置文件
.\VoiceAS.exe --configure --config "D:\Audio Settings\priority.txt"
.\VoiceAS.exe --monitor --config "D:\Audio Settings\priority.txt"
```

`--monitor --once` 会实际应用一次后退出；增加 `--dry-run` 则仅预览。操作失败时退出码为 1。

## 设备状态

输出为 UTF-8，支持中文设备名称。完整列表中的状态含义：

| 状态 | 含义 |
| --- | --- |
| Active | 当前可用 |
| Disabled | 已禁用 |
| Not present | 当前不存在 |
| Unplugged | 未插入 |

列表仅包含输出端点（如扬声器、耳机、HDMI 和虚拟输出设备），不包含麦克风。

## 代码入口

- `VoiceAS/EnumerateVoiceDevices.h`：设备结构与 `EnumerateAudioOutputDevices()` 声明。
- `VoiceAS/EnumerateVoiceDevices.cpp`：COM 生命周期、设备枚举、属性与默认角色查询；资源自动释放，API 失败时抛出包含 HRESULT 的异常。设备名称读取失败时显示占位名称。
- `VoiceAS/AudioPriority.h/.cpp`：优先级选择、输入解析、配置读取与保存。
- `VoiceAS/AudioMonitor.h/.cpp`：设备通知、默认设备切换、配置热更新、重试和单实例控制。
- `VoiceAS/WirelessHeadset.h/.cpp`：ROG Delta II HID 状态查询、回复校验及音频端点匹配。
- `VoiceAS/WindowsSupport.h`：COM、Windows 句柄和 UTF-8 转换辅助函数。
- `VoiceAS/VoiceAS.cpp`：菜单、配置界面和命令行入口。

枚举使用 Windows [IMMDeviceEnumerator::EnumAudioEndpoints](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-enumaudioendpoints)，参数为 `eRender` 和 `DEVICE_STATEMASK_ALL`。
监听使用 [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient)，回调只发送信号，避免在回调线程内进行阻塞操作。
默认设备设置使用未公开的 `IPolicyConfig::SetDefaultEndpoint`，集中封装在 `AudioMonitor.cpp`；其 ABI 参考 [PolicyConfig 接口声明](https://github.com/amate/SetDefaultAudioDevice/blob/master/PolicyConfig.h)，需在 Windows 版本升级后验证兼容性。
ROG 查询命令与回复格式参考 [G-Helper 的耳机状态实现](https://github.com/seerge/g-helper/blob/main/app/Peripherals/Headset/AsusHeadset.cs)。本机已分别在耳机关机和开机时验证：关机回复为 `CC 12 07 00 00 FF AA ...`，开机回复包含有效电量。

## 验证

在 solution 根目录的 Developer PowerShell 中执行：

```powershell
msbuild .\tests\VoiceAS.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64
.\tests\bin\x64\VoiceAS.Tests.exe

# 实际 COM 接口与通知注册测试：仅将每种角色重新设置到当前默认设备，保持现有路由
.\tests\bin\x64\VoiceAS.Tests.exe --integration

# PowerShell 7：配置界面、仅 Active 过滤、热更新、错误恢复和单实例验证
.\tests\SmokeTests.ps1
```

逻辑测试覆盖优先级、设备移除与重连、所有设备不可用、同名设备、错误输入和配置损坏；也用本机采集的 ROG 开/关机回复验证“端点 Active 但耳机关机”的降级与开机恢复，以及短包、错误报告 ID、无关回复与查询失败。
自动化测试不会控制耳机的物理电源。
