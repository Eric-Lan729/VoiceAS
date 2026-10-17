#include "AudioPriority.h"
#include "WindowsSupport.h"

#include <ShlObj.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace
{
    void ValidateConfig(const AudioPriorityConfig& config)
    {
        if (config.deviceIds.empty())
            throw std::runtime_error("Priority list is empty. Run --configure first.");
        std::set<std::wstring> seen;
        for (const auto& id : config.deviceIds)
        {
            if (id.empty() || id.find_first_of(L"\r\n") != std::wstring::npos
                || id.find(L'\0') != std::wstring::npos || !seen.insert(id).second)
                throw std::runtime_error("Configuration contains an empty, invalid, or repeated device ID.");
        }
    }
}

std::filesystem::path DefaultConfigPath()
{
    wchar_t* rawPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData,
        KF_FLAG_DONT_VERIFY, nullptr, &rawPath);
    const std::unique_ptr<wchar_t, windows_support::CoTaskMemDeleter> path(rawPath);
    windows_support::CheckResult(result, "SHGetKnownFolderPath");
    return std::filesystem::path(path.get()) / L"VoiceAS" / L"priority.txt";
}

AudioPriorityConfig LoadPriorityConfig(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open priority configuration. Run --configure first.");

    std::string line;
    const auto readLine = [&]()
    {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!readLine()) throw std::runtime_error("Empty priority configuration.");
    if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
    if (line != "VoiceAS priority v1") throw std::runtime_error("Unsupported priority configuration format.");

    AudioPriorityConfig config;
    if (!readLine() || (line != "communications=0" && line != "communications=1"))
        throw std::runtime_error("Invalid communications setting in priority configuration.");
    config.includeCommunications = line == "communications=1";
    while (readLine())
    {
        if (line.empty()) continue;
        if (!line.starts_with("device=")) throw std::runtime_error("Invalid priority configuration entry.");
        config.deviceIds.push_back(windows_support::FromUtf8(std::string_view(line).substr(7)));
    }
    if (input.bad()) throw std::runtime_error("Failed to read priority configuration.");
    ValidateConfig(config);
    return config;
}

void SavePriorityConfig(const std::filesystem::path& path, const AudioPriorityConfig& config)
{
    ValidateConfig(config);
    std::ostringstream content;
    content << "VoiceAS priority v1\ncommunications=" << (config.includeCommunications ? 1 : 0) << '\n';
    for (const auto& id : config.deviceIds)
        content << "device=" << windows_support::ToUtf8(id) << '\n';

    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    try
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write priority configuration.");
        output << content.str();
        output.close();
        if (!output) throw std::runtime_error("Failed to save priority configuration.");
        // Readers see either the complete old file or the complete new file.
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Save configuration");
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::vector<size_t> ParseDeviceOrder(std::string_view input, size_t deviceCount)
{
    std::string normalized(input);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream tokens(normalized);
    std::vector<size_t> result;
    std::set<size_t> seen;
    for (std::string token; tokens >> token;)
    {
        size_t number = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()
            || number == 0 || number > deviceCount || !seen.insert(number).second)
            throw std::runtime_error("Use distinct device numbers from the list, for example: 2 1 3.");
        result.push_back(number - 1);
    }
    if (result.empty()) throw std::runtime_error("Select at least one device.");
    return result;
}

std::optional<size_t> FindPreferredDevice(const AudioPriorityConfig& config,
    const std::vector<AudioOutputDevice>& devices)
{
    for (const auto& id : config.deviceIds)
        for (size_t index = 0; index < devices.size(); ++index)
            if (devices[index].id == id && devices[index].state == AudioDeviceState::Active
                && devices[index].wirelessState != WirelessConnectionState::Offline
                && devices[index].wirelessState != WirelessConnectionState::Unknown)
                return index;
    return std::nullopt;
}
