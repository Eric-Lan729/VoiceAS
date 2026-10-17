#pragma once

#include "EnumerateVoiceDevices.h"

#include <filesystem>

// Applies only roles which do not already point at this endpoint in the snapshot.
void SetDefaultOutputDevice(const AudioOutputDevice& device, bool includeCommunications);
void RunAudioMonitor(const std::filesystem::path& configPath, bool dryRun = false, bool once = false);
