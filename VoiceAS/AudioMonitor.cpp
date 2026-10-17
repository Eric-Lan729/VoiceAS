#include "AudioMonitor.h"
#include "AudioPriority.h"
#include "WindowsSupport.h"
#include "BackgroundMonitor.h"

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl.h>

#include <atomic>
#include <iostream>
#include <memory>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")

namespace
{
    using namespace windows_support;
    using Microsoft::WRL::ComPtr;

    // Undocumented Windows interface, isolated here because it is not in the SDK.
    // ABI reference: https://github.com/amate/SetDefaultAudioDevice/blob/master/PolicyConfig.h
    struct DeviceShareMode;
    struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
        virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
    };
    class __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9")) PolicyConfigClient;

    using SharedHandle = std::shared_ptr<void>;
    std::atomic<HANDLE> stopEvent{ nullptr };

    BOOL WINAPI HandleConsoleControl(DWORD control)
    {
        if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) return FALSE;
        const auto event = stopEvent.load();
        if (!event) return FALSE;
        SetEvent(event);
        return TRUE;
    }

    SharedHandle MakeEvent(bool manualReset, const wchar_t* name = nullptr)
    {
        SharedHandle event(CreateEventW(nullptr, manualReset, FALSE, name), HandleDeleter{});
        if (!event) throw std::runtime_error("Unable to create monitor event.");
        return event;
    }

    class ConsoleControlRegistration
    {
    public:
        explicit ConsoleControlRegistration(HANDLE event)
        {
            stopEvent.store(event);
            if (!SetConsoleCtrlHandler(HandleConsoleControl, TRUE))
            {
                stopEvent.store(nullptr);
                throw std::runtime_error("Unable to register Ctrl+C handler.");
            }
        }
        ~ConsoleControlRegistration()
        {
            SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
            stopEvent.store(nullptr);
        }
        ConsoleControlRegistration(const ConsoleControlRegistration&) = delete;
        ConsoleControlRegistration& operator=(const ConsoleControlRegistration&) = delete;
    };

    class DeviceNotifications final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient>
    {
    public:
        explicit DeviceNotifications(SharedHandle changed) : changed_(std::move(changed)) {}
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Signal(); }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Signal(); }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Signal(); }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return Signal(); }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
        {
            return flow == eRender ? Signal() : S_OK;
        }
    private:
        HRESULT Signal() noexcept
        {
            // Callback threads only signal; enumeration and switching run in the main loop.
            SetEvent(changed_.get());
            return S_OK;
        }
        SharedHandle changed_;
    };

    class NotificationRegistration
    {
    public:
        explicit NotificationRegistration(const SharedHandle& changed)
        {
            CheckResult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(enumerator_.GetAddressOf())), "Create device enumerator");
            notifications_ = Microsoft::WRL::Make<DeviceNotifications>(changed);
            if (!notifications_) throw std::bad_alloc();
            CheckResult(enumerator_->RegisterEndpointNotificationCallback(notifications_.Get()),
                "RegisterEndpointNotificationCallback");
        }
        ~NotificationRegistration()
        {
            enumerator_->UnregisterEndpointNotificationCallback(notifications_.Get());
        }
        NotificationRegistration(const NotificationRegistration&) = delete;
        NotificationRegistration& operator=(const NotificationRegistration&) = delete;
    private:
        ComPtr<IMMDeviceEnumerator> enumerator_;
        ComPtr<DeviceNotifications> notifications_;
    };
}

void SetDefaultOutputDevice(const AudioOutputDevice& device, bool includeCommunications)
{
    if (device.state != AudioDeviceState::Active || device.id.empty())
        throw std::runtime_error("The selected output device is not active.");
    const ComApartment apartment;
    ComPtr<IPolicyConfig> policy;
    CheckResult(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(policy.GetAddressOf())), "Create audio policy client");
    if (!device.isDefaultConsole)
        CheckResult(policy->SetDefaultEndpoint(device.id.c_str(), eConsole), "Set default console output");
    if (!device.isDefaultMultimedia)
        CheckResult(policy->SetDefaultEndpoint(device.id.c_str(), eMultimedia), "Set default multimedia output");
    if (includeCommunications && !device.isDefaultCommunications)
        CheckResult(policy->SetDefaultEndpoint(device.id.c_str(), eCommunications), "Set default communications output");
}

void RunAudioMonitor(const std::filesystem::path& configPath, bool dryRun, bool once,
    const std::wstring& readyEventName)
{
    auto config = LoadPriorityConfig(configPath); // Fail clearly before starting if never configured.
    const ComApartment apartment;
    UniqueHandle instance(CreateMutexW(nullptr, FALSE, MonitorMutexName));
    const DWORD instanceError = GetLastError();
    if (!instance) throw std::runtime_error("Unable to create monitor instance lock.");
    if (instanceError == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("An audio monitor is already running in this Windows session.");

    const auto stop = MakeEvent(true, MonitorStopEventName);
    if (!ResetEvent(stop.get())) ThrowWin32Error("Reset monitor stop event");
    const auto changed = MakeEvent(false);
    const ConsoleControlRegistration controls(stop.get());
    const NotificationRegistration notifications(changed);
    std::cout << "Monitor started" << (dryRun ? " (preview mode; no routing changes)" : "")
        << ". Use --stop to stop it; Ctrl+C also works in foreground mode.\n"
        << "Configuration: " << ToUtf8(configPath.wstring()) << '\n'
        << "Configuration changes are reloaded automatically. Only Active output endpoints are selected.\n" << std::flush;
    if (!readyEventName.empty())
    {
        const UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str()));
        if (!ready || !SetEvent(ready.get())) ThrowWin32Error("Report monitor startup");
    }

    std::string previousStatus;
    std::string previousConfigError;
    const auto report = [&](const std::string& status)
    {
        if (status != previousStatus)
        {
            std::cout << status << '\n' << std::flush;
            previousStatus = status;
        }
    };
    for (;;)
    {
        if (WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0) break;
        try
        {
            const auto updated = LoadPriorityConfig(configPath);
            if (updated != config)
            {
                config = updated;
                previousStatus.clear();
                std::cout << "Loaded updated priority order.\n" << std::flush;
            }
            if (!previousConfigError.empty()) std::cout << "Configuration reading recovered.\n" << std::flush;
            previousConfigError.clear();
        }
        catch (const std::exception& error)
        {
            if (once) throw;
            if (previousConfigError != error.what())
            {
                previousConfigError = error.what();
                std::cerr << "Cannot read configuration; keeping the last valid configuration: " << error.what() << '\n';
            }
        }

        try
        {
            const auto devices = EnumerateAudioOutputDevices();
            const auto preferred = FindPreferredDevice(config, devices);
            if (!preferred)
                report("No configured device is Active. Keeping the current default output and waiting for a connection.");
            else
            {
                const auto& device = devices[*preferred];
                const bool alreadyDefault = device.isDefaultConsole && device.isDefaultMultimedia
                    && (!config.includeCommunications || device.isDefaultCommunications);
                if (dryRun)
                    report("Preferred output: " + ToUtf8(device.name) + (alreadyDefault ? " (already default)" : " (would switch to this device)"));
                else
                {
                    if (!alreadyDefault)
                    {
                        SetDefaultOutputDevice(device, config.includeCommunications);
                        // Successful HRESULTs are followed by a fresh snapshot to verify routing.
                        const auto verified = EnumerateAudioOutputDevices();
                        bool applied = false;
                        for (const auto& current : verified)
                            if (current.id == device.id)
                                applied = current.isDefaultConsole && current.isDefaultMultimedia
                                    && (!config.includeCommunications || current.isDefaultCommunications);
                        if (!applied) throw std::runtime_error("Default output changed during switching; will retry.");
                        previousStatus.clear();
                    }
                    report("Current preferred output: " + ToUtf8(device.name));
                }
            }
        }
        catch (const std::exception& error)
        {
            if (once) throw;
            report(std::string("Audio operation failed; will retry: ") + error.what());
        }
        if (once) break;

        // Notifications handle hotplug immediately. The timeout retries transient failures,
        // reloads configuration, and recovers even when an audio-service event is missed.
        const HANDLE events[] = { stop.get(), changed.get() };
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) throw std::runtime_error("Failed to wait for audio device events.");
        if (result == WAIT_OBJECT_0 + 1 && WaitForSingleObject(stop.get(), 150) == WAIT_OBJECT_0) break;
    }
    std::cout << "Monitor stopped.\n";
}
