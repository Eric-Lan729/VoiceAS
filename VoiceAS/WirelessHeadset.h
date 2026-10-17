#pragma once

#include "EnumerateVoiceDevices.h"

#include <span>

struct WirelessHeadsetStatus
{
    std::wstring containerId;
    WirelessConnectionState state = WirelessConnectionState::Unknown;
    int batteryPercent = -1;
    std::string detail;
};

// The only command sent to the device is the ROG battery/readiness GET query.
WirelessHeadsetStatus DecodeRogStatus(std::span<const unsigned char> response);
std::vector<WirelessHeadsetStatus> QueryWirelessHeadsets();
void UpdateWirelessAvailability(std::vector<AudioOutputDevice>& devices);
const char* WirelessStateName(WirelessConnectionState state);
