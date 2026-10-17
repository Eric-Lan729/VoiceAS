#include "AudioMonitor.h"
#include "AudioPriority.h"
#include "EnumerateVoiceDevices.h"
#include "WindowsSupport.h"
#include "WirelessHeadset.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace
{
    using windows_support::ToUtf8;

    class ConsoleEncoding
    {
    public:
        ConsoleEncoding() : previous_(GetConsoleOutputCP()) { SetConsoleOutputCP(CP_UTF8); }
        ~ConsoleEncoding()
        {
            std::cout.flush();
            std::cerr.flush();
            if (previous_ != 0) SetConsoleOutputCP(previous_);
        }
    private:
        UINT previous_;
    };

    const char* StateName(AudioDeviceState state)
    {
        switch (state)
        {
        case AudioDeviceState::Active: return "Active";
        case AudioDeviceState::Disabled: return "Disabled";
        case AudioDeviceState::NotPresent: return "Not present";
        case AudioDeviceState::Unplugged: return "Unplugged";
        default: return "Unknown";
        }
    }

    void ListDevices()
    {
        auto devices = EnumerateAudioOutputDevices();
        UpdateWirelessAvailability(devices);
        std::cout << "Audio output devices: " << devices.size() << "\n\n";
        for (size_t index = 0; index < devices.size(); ++index)
        {
            const auto& device = devices[index];
            std::cout << '[' << index + 1 << "] " << ToUtf8(device.name) << '\n'
                << "    ID: " << ToUtf8(device.id) << '\n'
                << "    State: " << StateName(device.state) << '\n';
            if (device.wirelessState != WirelessConnectionState::NotApplicable)
                std::cout << "    Wireless: " << WirelessStateName(device.wirelessState) << '\n';
            if (device.isDefaultConsole) std::cout << "    Default: Console\n";
            if (device.isDefaultMultimedia) std::cout << "    Default: Multimedia\n";
            if (device.isDefaultCommunications) std::cout << "    Default: Communications\n";
            std::cout << '\n';
        }
    }

    void HeadsetStatus()
    {
        auto devices = EnumerateAudioOutputDevices();
        UpdateWirelessAvailability(devices);
        bool found = false;
        for (const auto& device : devices)
        {
            if (device.wirelessState == WirelessConnectionState::NotApplicable) continue;
            found = true;
            std::cout << ToUtf8(device.name) << "\n    Windows: " << StateName(device.state)
                << "\n    Wireless: " << WirelessStateName(device.wirelessState)
                << "\n    Container: " << ToUtf8(device.containerId)
                << "\n    Battery: " << device.batteryPercent
                << "\n    Detail: " << device.wirelessDetail << '\n';
        }
        if (!found)
        {
            std::cout << "No matching ROG wireless audio endpoint. HID diagnostics:\n";
            for (const auto& status : QueryWirelessHeadsets())
                std::cout << ToUtf8(status.containerId) << " " << WirelessStateName(status.state)
                    << " " << status.detail << '\n';
        }
    }

    void Configure(const std::filesystem::path& path)
    {
        AudioPriorityConfig previous;
        if (std::filesystem::exists(path))
        {
            try { previous = LoadPriorityConfig(path); }
            catch (const std::exception& error)
            {
                std::cerr << "旧配置无法读取，可以重新配置：" << error.what() << '\n';
            }
        }

        for (;;)
        {
            auto active = EnumerateAudioOutputDevices();
            UpdateWirelessAvailability(active);
            std::erase_if(active, [](const auto& device) { return device.state != AudioDeviceState::Active; });
            std::cout << "\n配置输出优先顺序（仅显示 Active 设备）\n"
                << "保存后替换原有顺序；请先连接所有需要加入的设备。\n\n";
            for (size_t index = 0; index < active.size(); ++index)
            {
                const auto& device = active[index];
                std::cout << '[' << index + 1 << "] " << ToUtf8(device.name);
                const auto saved = std::find(previous.deviceIds.begin(), previous.deviceIds.end(), device.id);
                if (saved != previous.deviceIds.end())
                    std::cout << "  [已保存优先级 " << std::distance(previous.deviceIds.begin(), saved) + 1 << ']';
                if (device.isDefaultConsole) std::cout << "  [默认播放]";
                if (device.isDefaultCommunications) std::cout << "  [默认通信]";
                if (device.wirelessState != WirelessConnectionState::NotApplicable)
                    std::cout << "  [耳机本体 " << WirelessStateName(device.wirelessState) << ']';
                std::cout << "\n    ID: " << ToUtf8(device.id) << '\n';
            }
            if (active.empty()) std::cout << "当前没有 Active 的输出设备，请连接或启用设备后刷新。\n";
            else std::cout << "无线耳机即使关机也可能保持 Active；监听时只选择本体 Online 的 ROG 耳机。\n";
            std::cout << "\n按优先级从高到低输入序号，例如 2 1 3（也支持逗号）。\n"
                << "可只选部分设备；未选择的设备不参与自动切换。\n"
                << "输入 r 刷新，q 或直接回车取消：" << std::flush;
            std::string input;
            if (!std::getline(std::cin, input) || input.empty() || input == "q" || input == "Q")
            {
                std::cout << "已取消，原配置保持不变。\n";
                return;
            }
            if (input == "r" || input == "R") continue;

            std::vector<size_t> order;
            try { order = ParseDeviceOrder(input, active.size()); }
            catch (const std::exception& error)
            {
                std::cerr << "输入无效：" << error.what() << '\n';
                continue;
            }

            const auto refreshed = EnumerateAudioOutputDevices();
            const bool stillActive = std::all_of(order.begin(), order.end(), [&](size_t index)
            {
                return std::any_of(refreshed.begin(), refreshed.end(), [&](const auto& device)
                {
                    return device.id == active[index].id && device.state == AudioDeviceState::Active;
                });
            });
            if (!stillActive)
            {
                std::cout << "选择的设备状态已经改变，已刷新列表，请重新选择。\n";
                continue;
            }

            AudioPriorityConfig config;
            config.includeCommunications = previous.includeCommunications;
            for (const auto index : order) config.deviceIds.push_back(active[index].id);
            for (;;)
            {
                std::cout << "是否同时切换通信设备（语音/通话）？"
                    << (previous.includeCommunications ? " [Y/n]：" : " [y/N]：") << std::flush;
                if (!std::getline(std::cin, input))
                {
                    std::cout << "\n已取消，原配置保持不变。\n";
                    return;
                }
                if (input.empty()) break;
                if (input == "y" || input == "Y") { config.includeCommunications = true; break; }
                if (input == "n" || input == "N") { config.includeCommunications = false; break; }
                std::cout << "请输入 y 或 n。\n";
            }

            SavePriorityConfig(path, config);
            std::cout << "\n优先顺序已保存：\n";
            for (size_t priority = 0; priority < order.size(); ++priority)
                std::cout << priority + 1 << ". " << ToUtf8(active[order[priority]].name) << '\n';
            std::cout << "通信设备：" << (config.includeCommunications ? "跟随切换" : "保持独立")
                << "\n配置文件：" << ToUtf8(path.wstring()) << "\n打开监听入口即可开始自动切换。\n";
            return;
        }
    }

    void PrintHelp()
    {
        std::cout << "VoiceAS - 音频输出优先级切换\n\n"
            << "VoiceAS.exe                打开菜单\n"
            << "VoiceAS.exe --configure    配置优先顺序（仅 Active 设备）\n"
            << "VoiceAS.exe --monitor      开始监听并自动切换；Ctrl+C 停止\n"
            << "VoiceAS.exe --list         列出所有状态的输出设备\n"
            << "VoiceAS.exe --headset-status  查询 ROG 耳机本体连接状态\n"
            << "VoiceAS.exe --monitor --dry-run          预览监听，不修改默认设备\n"
            << "VoiceAS.exe --monitor --dry-run --once   检查一次后退出\n"
            << "附加参数：--config <路径> 指定配置文件\n";
    }

    void Menu(const std::filesystem::path& path)
    {
        for (;;)
        {
            std::cout << "\nVoiceAS\n1. 配置设备优先顺序\n2. 开始监听并自动切换\n3. 列出所有输出设备\n0. 退出\n选择：" << std::flush;
            std::string choice;
            if (!std::getline(std::cin, choice) || choice == "0") return;
            try
            {
                if (choice == "1") Configure(path);
                else if (choice == "2") RunAudioMonitor(path);
                else if (choice == "3") ListDevices();
                else std::cout << "请输入 0、1、2 或 3。\n";
            }
            catch (const std::exception& error) { std::cerr << "操作失败：" << error.what() << '\n'; }
            if (!std::cin) return;
        }
    }
}

int wmain(int argc, wchar_t* argv[])
{
    const ConsoleEncoding encoding;
    try
    {
        std::wstring command;
        std::filesystem::path configPath;
        bool dryRun = false;
        bool once = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring argument = argv[index];
            if (argument == L"--config")
            {
                if (++index == argc || !configPath.empty()) throw std::runtime_error("Use --config <path> once.");
                configPath = argv[index];
                if (configPath.empty()) throw std::runtime_error("Configuration path cannot be empty.");
            }
            else if (argument == L"--dry-run") dryRun = true;
            else if (argument == L"--once") once = true;
            else if (argument == L"--configure" || argument == L"--monitor" || argument == L"--list"
                || argument == L"--headset-status" || argument == L"--help" || argument == L"-h")
            {
                if (!command.empty()) throw std::runtime_error("Choose one command; see --help.");
                command = argument;
            }
            else throw std::runtime_error("Unknown argument; see --help.");
        }
        if ((dryRun || once) && command != L"--monitor")
            throw std::runtime_error("--dry-run and --once require --monitor.");
        if (command == L"--help" || command == L"-h") { PrintHelp(); return 0; }
        if (command == L"--list") { ListDevices(); return 0; }
        if (command == L"--headset-status") { HeadsetStatus(); return 0; }
        if (configPath.empty()) configPath = DefaultConfigPath();
        configPath = std::filesystem::absolute(configPath);
        if (command == L"--configure") Configure(configPath);
        else if (command == L"--monitor") RunAudioMonitor(configPath, dryRun, once);
        else Menu(configPath);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "操作失败：" << error.what() << '\n';
        return 1;
    }
}
