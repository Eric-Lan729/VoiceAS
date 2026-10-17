#pragma once

#include <filesystem>

inline constexpr wchar_t MonitorMutexName[] = L"Local\\VoiceAS.AudioOutputMonitor";
inline constexpr wchar_t MonitorStopEventName[] = L"Local\\VoiceAS.AudioOutputMonitor.Stop";

bool IsAudioMonitorRunning();
std::filesystem::path MonitorLogPath(const std::filesystem::path& configPath);
void StartBackgroundMonitor(const std::filesystem::path& configPath, bool dryRun = false);
void StopAudioMonitor();
