#pragma once

#include <string>
#include <vector>

enum class AudioDeviceState
{
    Active,
    Disabled,
    NotPresent,
    Unplugged,
    Unknown
};

enum class WirelessConnectionState
{
    NotApplicable,
    Online,
    Offline,
    Unknown
};

struct AudioOutputDevice
{
    std::wstring id;
    std::wstring name;
    AudioDeviceState state = AudioDeviceState::Unknown;
    bool isDefaultConsole = false;
    bool isDefaultMultimedia = false;
    bool isDefaultCommunications = false;
    std::wstring containerId;
    WirelessConnectionState wirelessState = WirelessConnectionState::NotApplicable;
    int batteryPercent = -1;
    std::string wirelessDetail;
};

// Takes a snapshot of all render endpoints, including inactive devices.
// Initializes COM for this call and throws std::runtime_error on API failure.
std::vector<AudioOutputDevice> EnumerateAudioOutputDevices();
