#include "BackgroundMonitor.h"
#include "AudioPriority.h"
#include "WindowsSupport.h"

#include <iostream>
#include <vector>

namespace
{
    using namespace windows_support;

    std::wstring ExecutablePath()
    {
        std::vector<wchar_t> buffer(512);
        for (;;)
        {
            const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0) ThrowWin32Error("Find executable");
            if (size < buffer.size()) return {buffer.data(), size};
            if (buffer.size() >= 32768) throw std::runtime_error("Executable path is too long.");
            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring QuoteArgument(std::wstring_view value)
    {
        std::wstring result = L"\"";
        size_t slashes = 0;
        for (const wchar_t letter : value)
        {
            if (letter == L'\\') { ++slashes; continue; }
            result.append(letter == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
            result += letter;
            slashes = 0;
        }
        result.append(2 * slashes, L'\\');
        result += L'\"';
        return result;
    }

    class InheritedHandles
    {
    public:
        InheritedHandles(HANDLE* handles, size_t count)
        {
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            if (size == 0) ThrowWin32Error("Size process attributes");
            storage_.resize(size);
            attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
            if (!InitializeProcThreadAttributeList(attributes_, 1, 0, &size))
                ThrowWin32Error("Initialize process attributes");
            if (!UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles, sizeof(HANDLE) * count, nullptr, nullptr))
            {
                const DWORD error = GetLastError();
                DeleteProcThreadAttributeList(attributes_);
                ThrowWin32Error("Set inherited handles", error);
            }
        }
        ~InheritedHandles() { DeleteProcThreadAttributeList(attributes_); }
        LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return attributes_; }
        InheritedHandles(const InheritedHandles&) = delete;
        InheritedHandles& operator=(const InheritedHandles&) = delete;
    private:
        std::vector<unsigned char> storage_;
        LPPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
    };
}

bool IsAudioMonitorRunning()
{
    const windows_support::UniqueHandle instance(OpenMutexW(SYNCHRONIZE, FALSE, MonitorMutexName));
    if (instance) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return false;
    windows_support::ThrowWin32Error("Check running monitor", error);
}

std::filesystem::path MonitorLogPath(const std::filesystem::path& configPath)
{
    return configPath.parent_path() / L"monitor.log";
}

void StartBackgroundMonitor(const std::filesystem::path& configPath, bool dryRun)
{
    using namespace windows_support;
    LoadPriorityConfig(configPath);
    if (IsAudioMonitorRunning())
        throw std::runtime_error("An audio monitor is already running. Use --stop before starting another.");

    const auto logPath = MonitorLogPath(configPath);
    std::filesystem::create_directories(logPath.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE rawLog = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawLog == INVALID_HANDLE_VALUE) ThrowWin32Error("Open monitor log");
    const UniqueHandle log(rawLog);
    const HANDLE rawInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawInput == INVALID_HANDLE_VALUE) ThrowWin32Error("Open background input");
    const UniqueHandle input(rawInput);

    const auto readyName = L"Local\\VoiceAS.Ready." + std::to_wstring(GetCurrentProcessId())
        + L"." + std::to_wstring(GetTickCount64());
    const UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    if (!ready) ThrowWin32Error("Create startup event");

    const auto executable = ExecutablePath();
    auto commandLine = QuoteArgument(executable) + L" --monitor-worker --config "
        + QuoteArgument(std::filesystem::absolute(configPath).wstring())
        + L" --ready-event " + QuoteArgument(readyName);
    if (dryRun) commandLine += L" --dry-run";

    // Only the log and NUL are inherited: never keep the launcher's terminal or pipes alive.
    HANDLE handles[]{log.get(), input.get()};
    InheritedHandles inherited(handles, 2);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = log.get();
    startup.StartupInfo.hStdError = log.get();
    startup.lpAttributeList = inherited.Get();
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &processInfo))
        ThrowWin32Error("Start background monitor");
    const UniqueHandle process(processInfo.hProcess);
    const UniqueHandle thread(processInfo.hThread);

    const HANDLE startupEvents[]{ready.get(), process.get()};
    const DWORD result = WaitForMultipleObjects(2, startupEvents, FALSE, 10000);
    if (result != WAIT_OBJECT_0)
    {
        // This child belongs to this launch attempt and has not reported readiness.
        if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 2000);
        }
        throw std::runtime_error("Background monitor failed to start. See log: " + ToUtf8(logPath.wstring()));
    }
    std::cout << "Background monitor started (PID " << processInfo.dwProcessId << ").\n"
        << "Log: " << ToUtf8(logPath.wstring()) << "\nUse --stop or StopMonitor.cmd to stop it.\n";
}

void StopAudioMonitor()
{
    using namespace windows_support;
    if (!IsAudioMonitorRunning())
    {
        std::cout << "No audio monitor is running.\n";
        return;
    }
    const UniqueHandle stop(OpenEventW(EVENT_MODIFY_STATE, FALSE, MonitorStopEventName));
    if (!stop)
        throw std::runtime_error("Cannot signal this monitor. If it is an older version, close its console window first.");
    if (!SetEvent(stop.get())) ThrowWin32Error("Signal monitor stop");
    const auto deadline = GetTickCount64() + 10000;
    while (IsAudioMonitorRunning() && GetTickCount64() < deadline) Sleep(50);
    if (IsAudioMonitorRunning()) throw std::runtime_error("Monitor has not stopped yet; try again shortly.");
    std::cout << "Audio monitor stopped.\n";
}
