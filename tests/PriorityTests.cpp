#include "../VoiceAS/AudioMonitor.h"
#include "../VoiceAS/AudioPriority.h"
#include "../VoiceAS/WindowsSupport.h"
#include "../VoiceAS/WirelessHeadset.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>

namespace
{
    int checks = 0;
    void Require(bool condition, const char* description)
    {
        if (!condition) throw std::runtime_error(description);
        ++checks;
    }

    template<typename Action> void Reject(Action action, const char* description)
    {
        bool rejected = false;
        try { action(); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, description);
    }

    struct TestFiles
    {
        std::filesystem::path directory = std::filesystem::current_path() / L"tests"
            / (L"test-output-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::path config = directory / L"priority.txt";
        TestFiles() { std::filesystem::create_directories(directory); }
        ~TestFiles()
        {
            std::error_code ignored;
            std::filesystem::remove(config, ignored);
            std::filesystem::remove(directory, ignored);
        }
        void Write(const std::string& text) const
        {
            std::ofstream output(config, std::ios::binary | std::ios::trunc);
            output << text;
        }
    };

    void TestPriorities()
    {
        AudioPriorityConfig config{ { L"headphones", L"speakers" }, false };
        std::vector<AudioOutputDevice> devices{
            {L"unconfigured", L"same name", AudioDeviceState::Active},
            {L"speakers", L"same name", AudioDeviceState::Active},
            {L"headphones", L"headphones", AudioDeviceState::Active}
        };
        Require(FindPreferredDevice(config, devices) == size_t{2}, "Priority must override enumeration order.");
        for (const auto state : {AudioDeviceState::Disabled, AudioDeviceState::Unplugged,
            AudioDeviceState::NotPresent, AudioDeviceState::Unknown})
        {
            devices[2].state = state;
            Require(FindPreferredDevice(config, devices) == size_t{1}, "Unavailable first choice must fall back.");
        }
        devices.pop_back();
        Require(FindPreferredDevice(config, devices) == size_t{1}, "Removed device must fall back.");
        devices.push_back({ L"headphones", L"renamed headphones", AudioDeviceState::Active });
        Require(FindPreferredDevice(config, devices) == size_t{2}, "Reconnected first choice must regain priority by ID.");
        devices[1].state = AudioDeviceState::Disabled;
        devices[2].state = AudioDeviceState::Unplugged;
        Require(!FindPreferredDevice(config, devices), "Unconfigured active devices must not be selected.");
        Require(!FindPreferredDevice(config, {}), "No endpoints must be handled.");
        Require(!FindPreferredDevice({}, devices), "No priority entries must select nothing.");
    }

    void TestOrderInput()
    {
        Require(ParseDeviceOrder("3 1 2", 3) == std::vector<size_t>{2, 0, 1}, "Space-delimited input.");
        Require(ParseDeviceOrder("2, 1", 3) == std::vector<size_t>{1, 0}, "Comma-delimited subset.");
        Require(ParseDeviceOrder(" 1 ", 3) == std::vector<size_t>{0}, "Single selected endpoint.");
        for (const auto* input : {"", " ", "0", "4", "1 1", "-1", "1x", "1.5", "1/2", "999999999999999999999999"})
            Reject([&] { ParseDeviceOrder(input, 3); }, "Invalid order must be rejected.");
        Reject([] { ParseDeviceOrder("1", 0); }, "Empty active list must reject selection.");
    }

    void TestWirelessReadiness()
    {
        // Captured from this ROG Delta II with its receiver still plugged in.
        const std::array<unsigned char, 16> off{0xCC, 0x12, 0x07, 0, 0, 0xFF, 0xAA};
        const std::array<unsigned char, 16> on{0xCC, 0x12, 0x07, 0, 0, 0x05, 0x56, 0x14, 1};
        Require(DecodeRogStatus(off).state == WirelessConnectionState::Offline, "Powered-off headset must be offline despite Active endpoint.");
        Require(DecodeRogStatus(on).state == WirelessConnectionState::Online, "Powered-on headset must be online.");
        Require(DecodeRogStatus(on).batteryPercent == 86, "Battery byte must be decoded correctly.");
        Require(DecodeRogStatus({}).state == WirelessConnectionState::Unknown, "Empty HID response must not imply power-off.");
        Require(DecodeRogStatus(std::span(on).first(7)).state == WirelessConnectionState::Unknown, "Truncated responses must not imply power-on.");
        auto packet = on;
        packet[2] = 0x03;
        Require(DecodeRogStatus(packet).state == WirelessConnectionState::Unknown, "Unrelated HID responses must be ignored.");
        packet = on;
        packet[0] = 0;
        Require(DecodeRogStatus(packet).state == WirelessConnectionState::Unknown, "Wrong report ID must not be accepted.");
        packet = on;
        packet[6] = 0;
        Require(DecodeRogStatus(packet).state == WirelessConnectionState::Online, "Zero battery alone must not imply power-off.");
        packet[6] = 100;
        Require(DecodeRogStatus(packet).batteryPercent == 100, "Full battery must be supported.");
        packet[6] = 0xFF;
        Require(DecodeRogStatus(packet).state == WirelessConnectionState::Unknown, "Invalid battery values must not be mistaken for a known power state.");

        AudioPriorityConfig config{{L"wireless", L"speakers"}, false};
        std::vector<AudioOutputDevice> devices{
            {L"wireless", L"ROG", AudioDeviceState::Active},
            {L"speakers", L"Speakers", AudioDeviceState::Active}
        };
        devices[0].wirelessState = DecodeRogStatus(off).state;
        Require(FindPreferredDevice(config, devices) == size_t{1}, "Power-off must select next configured output.");
        devices[0].wirelessState = DecodeRogStatus(on).state;
        Require(FindPreferredDevice(config, devices) == size_t{0}, "Power-on must regain first priority.");
        devices[0].wirelessState = WirelessConnectionState::Unknown;
        Require(FindPreferredDevice(config, devices) == size_t{1}, "Read failure must not be treated as a confirmed online headset.");
        devices[1].state = AudioDeviceState::Disabled;
        Require(!FindPreferredDevice(config, devices), "Unknown headset plus unavailable fallback must select nothing.");
    }

    void TestConfiguration()
    {
        TestFiles files;
        Reject([&] { LoadPriorityConfig(files.config); }, "Missing configuration must be rejected.");
        const AudioPriorityConfig expected{ {L"opaque-device-id-耳机", L"second-id"}, true };
        SavePriorityConfig(files.config, expected);
        Require(LoadPriorityConfig(files.config) == expected, "Order, Unicode IDs and role option must round-trip.");
        const AudioPriorityConfig replacement{ {L"new-id"}, false };
        SavePriorityConfig(files.config, replacement);
        Require(LoadPriorityConfig(files.config) == replacement, "New order must replace old order atomically.");
        Reject([&] { SavePriorityConfig(files.config, {}); }, "Empty order must not overwrite configuration.");
        Reject([&] { SavePriorityConfig(files.config, {{L"duplicate", L"duplicate"}, false}); }, "Repeated IDs must be rejected.");
        Reject([&] { SavePriorityConfig(files.config, {{L"line\nbreak"}, false}); }, "Newlines must not inject configuration.");
        Require(LoadPriorityConfig(files.config) == replacement, "Invalid save must preserve previous configuration.");
        for (const auto* content : {"", "VoiceAS priority v2\n", "VoiceAS priority v1\ncommunications=yes\ndevice=a\n",
            "VoiceAS priority v1\ncommunications=0\n", "VoiceAS priority v1\ncommunications=0\ndevice=a\ndevice=a\n",
            "VoiceAS priority v1\ncommunications=0\ndevice=\n", "VoiceAS priority v1\ncommunications=0\nunknown=value\n",
            "VoiceAS priority v1\ncommunications=0\ndevice=\xFF\n"})
        {
            files.Write(content);
            Reject([&] { LoadPriorityConfig(files.config); }, "Corrupt configuration must be rejected.");
        }
        files.Write("\xEF\xBB\xBFVoiceAS priority v1\r\ncommunications=1\r\ndevice=a\r\n\r\n");
        Require(LoadPriorityConfig(files.config) == AudioPriorityConfig{{L"a"}, true}, "BOM and Windows line endings supported.");
    }

    void TestPolicyIntegration()
    {
        const auto before = EnumerateAudioOutputDevices();
        int reapplied = 0;
        // Exercise the real COM ABI by reapplying each role to its CURRENT device.
        // This deliberately preserves the user's current routing, including split roles.
        for (const auto& device : before)
        {
            for (int role = 0; role < 3; ++role)
            {
                const bool isCurrent = role == 0 ? device.isDefaultConsole
                    : role == 1 ? device.isDefaultMultimedia : device.isDefaultCommunications;
                if (!isCurrent) continue;
                auto current = device;
                current.isDefaultConsole = role != 0;
                current.isDefaultMultimedia = role != 1;
                current.isDefaultCommunications = role != 2;
                SetDefaultOutputDevice(current, true);
                ++reapplied;
            }
        }
        Require(reapplied == 3, "Integration test requires defaults for all three roles.");
        const auto after = EnumerateAudioOutputDevices();
        for (const auto& original : before)
        {
            if (!original.isDefaultConsole && !original.isDefaultMultimedia && !original.isDefaultCommunications) continue;
            bool preserved = false;
            for (const auto& device : after)
                if (device.id == original.id)
                    preserved = device.isDefaultConsole == original.isDefaultConsole
                        && device.isDefaultMultimedia == original.isDefaultMultimedia
                        && device.isDefaultCommunications == original.isDefaultCommunications;
            Require(preserved, "Integration test must preserve every current default role.");
        }

        const AudioOutputDevice* preferred = nullptr;
        for (const auto& device : before) if (device.isDefaultConsole) preferred = &device;
        Require(preferred != nullptr, "A console default must exist.");
        TestFiles files;
        SavePriorityConfig(files.config, {{preferred->id}, false});
        RunAudioMonitor(files.config, true, true); // Register, inspect, unregister and clean up.
        SavePriorityConfig(files.config, {{L"not-connected"}, false});
        RunAudioMonitor(files.config, true, true); // No matching device is a normal wait state.
        RunAudioMonitor(files.config, true, true); // Previous instance lock must have been released.
        ++checks;
    }
}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        TestPriorities();
        TestOrderInput();
        TestWirelessReadiness();
        TestConfiguration();
        if (argc > 1 && std::string_view(argv[1]) == "--integration") TestPolicyIntegration();
        std::cout << "PASS: " << checks << " checks.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
