#include "AudioMonitor.h"
#include "AudioPriority.h"
#include "EnumerateVoiceDevices.h"
#include "WindowsSupport.h"
#include "BackgroundMonitor.h"

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
        const auto devices = EnumerateAudioOutputDevices();
        std::cout << "Audio output devices: " << devices.size() << "\n\n";
        for (size_t index = 0; index < devices.size(); ++index)
        {
            const auto& device = devices[index];
            std::cout << '[' << index + 1 << "] " << ToUtf8(device.name) << '\n'
                << "    ID: " << ToUtf8(device.id) << '\n'
                << "    State: " << StateName(device.state) << '\n';
            if (device.isDefaultConsole) std::cout << "    Default: Console\n";
            if (device.isDefaultMultimedia) std::cout << "    Default: Multimedia\n";
            if (device.isDefaultCommunications) std::cout << "    Default: Communications\n";
            std::cout << '\n';
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
                std::cerr << "Cannot read the previous configuration; you can replace it: " << error.what() << '\n';
            }
        }

        for (;;)
        {
            auto active = EnumerateAudioOutputDevices();
            std::erase_if(active, [](const auto& device) { return device.state != AudioDeviceState::Active; });
            std::cout << "\nConfigure output priority (Active endpoints only)\n"
                << "Saving replaces the previous order. Connect all receivers/devices you want to include first.\n\n";
            for (size_t index = 0; index < active.size(); ++index)
            {
                const auto& device = active[index];
                std::cout << '[' << index + 1 << "] " << ToUtf8(device.name);
                const auto saved = std::find(previous.deviceIds.begin(), previous.deviceIds.end(), device.id);
                if (saved != previous.deviceIds.end())
                    std::cout << "  [saved priority " << std::distance(previous.deviceIds.begin(), saved) + 1 << ']';
                if (device.isDefaultConsole) std::cout << "  [default playback]";
                if (device.isDefaultCommunications) std::cout << "  [default communications]";
                std::cout << "\n    ID: " << ToUtf8(device.id) << '\n';
            }
            if (active.empty()) std::cout << "No Active output endpoints. Connect or enable a device, then refresh.\n";
            std::cout << "\nEnter device numbers from highest to lowest priority, e.g. 2 1 3 (commas also work).\n"
                << "You may select a subset; unselected devices are not used for automatic switching.\n"
                << "Enter r to refresh, q or an empty line to cancel: " << std::flush;
            std::string input;
            if (!std::getline(std::cin, input) || input.empty() || input == "q" || input == "Q")
            {
                std::cout << "Cancelled. The previous configuration was not changed.\n";
                return;
            }
            if (input == "r" || input == "R") continue;

            std::vector<size_t> order;
            try { order = ParseDeviceOrder(input, active.size()); }
            catch (const std::exception& error)
            {
                std::cerr << "Invalid input: " << error.what() << '\n';
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
                std::cout << "A selected endpoint changed state. Refreshing the list; please select again.\n";
                continue;
            }

            AudioPriorityConfig config;
            config.includeCommunications = previous.includeCommunications;
            for (const auto index : order) config.deviceIds.push_back(active[index].id);
            for (;;)
            {
                std::cout << "Also switch the communications output (voice/calls)?"
                    << (previous.includeCommunications ? " [Y/n]: " : " [y/N]: ") << std::flush;
                if (!std::getline(std::cin, input))
                {
                    std::cout << "\nCancelled. The previous configuration was not changed.\n";
                    return;
                }
                if (input.empty()) break;
                if (input == "y" || input == "Y") { config.includeCommunications = true; break; }
                if (input == "n" || input == "N") { config.includeCommunications = false; break; }
                std::cout << "Please enter y or n.\n";
            }

            SavePriorityConfig(path, config);
            std::cout << "\nPriority order saved:\n";
            for (size_t priority = 0; priority < order.size(); ++priority)
                std::cout << priority + 1 << ". " << ToUtf8(active[order[priority]].name) << '\n';
            std::cout << "Communications output: " << (config.includeCommunications ? "follows priority order" : "kept independent")
                << "\nConfiguration: " << ToUtf8(path.wstring()) << "\nRun Monitor.cmd to start automatic switching in the background.\n";
            return;
        }
    }

    void PrintHelp()
    {
        std::cout << "VoiceAS - audio output priority switching\n\n"
            << "VoiceAS.exe                Open the menu\n"
            << "VoiceAS.exe --configure    Configure priority (Active endpoints only)\n"
            << "VoiceAS.exe --monitor      Start monitoring in the background\n"
            << "VoiceAS.exe --stop         Stop the running monitor\n"
            << "VoiceAS.exe --status       Check whether a monitor is running\n"
            << "VoiceAS.exe --list         List output endpoints in every state\n"
            << "VoiceAS.exe --monitor --foreground      Monitor in this console; Ctrl+C to stop\n"
            << "VoiceAS.exe --monitor --foreground --dry-run   Preview without changing outputs\n"
            << "VoiceAS.exe --monitor --dry-run --once   Inspect once and exit\n"
            << "Optional: --config <path> selects a configuration file\n";
    }

    void Menu(const std::filesystem::path& path)
    {
        for (;;)
        {
            std::cout << "\nVoiceAS\n1. Configure output priority\n2. Start background monitor\n3. List all output devices\n4. Stop monitor\n0. Exit\nChoose: " << std::flush;
            std::string choice;
            if (!std::getline(std::cin, choice) || choice == "0") return;
            try
            {
                if (choice == "1") Configure(path);
                else if (choice == "2") StartBackgroundMonitor(path);
                else if (choice == "3") ListDevices();
                else if (choice == "4") StopAudioMonitor();
                else std::cout << "Please enter 0, 1, 2, 3 or 4.\n";
            }
            catch (const std::exception& error) { std::cerr << "Operation failed: " << error.what() << '\n'; }
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
        bool foreground = false;
        std::wstring readyEventName;
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
            else if (argument == L"--foreground") foreground = true;
            else if (argument == L"--ready-event")
            {
                if (++index == argc || !readyEventName.empty()) throw std::runtime_error("Invalid startup event argument.");
                readyEventName = argv[index];
            }
            else if (argument == L"--configure" || argument == L"--monitor" || argument == L"--list"
                || argument == L"--help" || argument == L"-h"
                || argument == L"--monitor-worker" || argument == L"--stop" || argument == L"--status")
            {
                if (!command.empty()) throw std::runtime_error("Choose one command; see --help.");
                command = argument;
            }
            else throw std::runtime_error("Unknown argument; see --help.");
        }
        if ((dryRun || once || foreground) && command != L"--monitor" && command != L"--monitor-worker")
            throw std::runtime_error("--dry-run, --once and --foreground require --monitor.");
        if (command == L"--monitor-worker" && (readyEventName.empty() || once || foreground))
            throw std::runtime_error("Invalid internal monitor startup arguments.");
        if (!readyEventName.empty() && command != L"--monitor-worker")
            throw std::runtime_error("Startup events are only used by the background worker.");
        if (command == L"--help" || command == L"-h") { PrintHelp(); return 0; }
        if (command == L"--list") { ListDevices(); return 0; }
        if (command == L"--stop") { StopAudioMonitor(); return 0; }
        if (command == L"--status")
        {
            const bool running = IsAudioMonitorRunning();
            std::cout << (running ? "Audio monitor is running.\n" : "No audio monitor is running.\n");
            return running ? 0 : 1;
        }
        if (configPath.empty()) configPath = DefaultConfigPath();
        configPath = std::filesystem::absolute(configPath);
        if (command == L"--configure") Configure(configPath);
        else if (command == L"--monitor-worker")
        {
            if (GetConsoleWindow() != nullptr)
                throw std::runtime_error("The background worker must run without a console window.");
            std::cout << "Background mode: no console window.\n";
            RunAudioMonitor(configPath, dryRun, false, readyEventName);
        }
        else if (command == L"--monitor")
        {
            if (foreground || once) RunAudioMonitor(configPath, dryRun, once);
            else StartBackgroundMonitor(configPath, dryRun);
        }
        else Menu(configPath);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Operation failed: " << error.what() << '\n';
        return 1;
    }
}
