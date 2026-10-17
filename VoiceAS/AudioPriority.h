#pragma once

#include "EnumerateVoiceDevices.h"

#include <filesystem>
#include <optional>
#include <string_view>

struct AudioPriorityConfig
{
    std::vector<std::wstring> deviceIds; // Highest priority first; IDs are opaque.
    bool includeCommunications = false;
    bool operator==(const AudioPriorityConfig&) const = default;
};

std::filesystem::path DefaultConfigPath();
AudioPriorityConfig LoadPriorityConfig(const std::filesystem::path& path);
void SavePriorityConfig(const std::filesystem::path& path, const AudioPriorityConfig& config);

// Returns zero-based indices. Rejects empty, repeated, malformed, or out-of-range input.
std::vector<size_t> ParseDeviceOrder(std::string_view input, size_t deviceCount);
std::optional<size_t> FindPreferredDevice(const AudioPriorityConfig& config,
    const std::vector<AudioOutputDevice>& devices);
