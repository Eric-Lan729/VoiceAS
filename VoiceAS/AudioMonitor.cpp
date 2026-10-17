#include "AudioMonitor.h"
#include "AudioPriority.h"
#include "WindowsSupport.h"
#include "WirelessHeadset.h"

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <map>
#include <algorithm>

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

    SharedHandle MakeEvent(bool manualReset)
    {
        SharedHandle event(CreateEventW(nullptr, manualReset, FALSE, nullptr), HandleDeleter{});
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

void RunAudioMonitor(const std::filesystem::path& configPath, bool dryRun, bool once)
{
    auto config = LoadPriorityConfig(configPath); // Fail clearly before starting if never configured.
    const ComApartment apartment;
    UniqueHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\VoiceAS.AudioOutputMonitor"));
    const DWORD instanceError = GetLastError();
    if (!instance) throw std::runtime_error("Unable to create monitor instance lock.");
    if (instanceError == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("An audio monitor is already running in this Windows session.");

    const auto stop = MakeEvent(true);
    const auto changed = MakeEvent(false);
    const ConsoleControlRegistration controls(stop.get());
    const NotificationRegistration notifications(changed);
    std::cout << "监听已启动" << (dryRun ? "（预览模式，不切换设备）" : "") << "。Ctrl+C 停止。\n"
        << "配置文件：" << ToUtf8(configPath.wstring()) << '\n'
        << "配置更新后自动生效；ROG 无线耳机需同时处于 Online 状态。\n" << std::flush;

    std::string previousStatus;
    std::string previousConfigError;
    std::map<std::wstring, WirelessConnectionState> previousWirelessStates;
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
                std::cout << "已加载新的优先顺序。\n" << std::flush;
            }
            if (!previousConfigError.empty()) std::cout << "配置文件读取已恢复。\n" << std::flush;
            previousConfigError.clear();
        }
        catch (const std::exception& error)
        {
            if (once) throw;
            if (previousConfigError != error.what())
            {
                previousConfigError = error.what();
                std::cerr << "配置读取失败，继续使用上次有效配置：" << error.what() << '\n';
            }
        }

        try
        {
            auto devices = EnumerateAudioOutputDevices();
            UpdateWirelessAvailability(devices);
            for (const auto& device : devices)
            {
                if (device.wirelessState == WirelessConnectionState::NotApplicable
                    || std::find(config.deviceIds.begin(), config.deviceIds.end(), device.id) == config.deviceIds.end())
                    continue;
                const auto previous = previousWirelessStates.find(device.id);
                if (previous == previousWirelessStates.end() || previous->second != device.wirelessState)
                {
                    std::cout << "无线连接：" << ToUtf8(device.name) << " -> " << WirelessStateName(device.wirelessState);
                    if (device.wirelessState == WirelessConnectionState::Unknown)
                        std::cout << "（状态查询失败，暂不选择）：" << device.wirelessDetail;
                    std::cout << '\n' << std::flush;
                    previousWirelessStates[device.id] = device.wirelessState;
                }
            }
            const auto preferred = FindPreferredDevice(config, devices);
            if (!preferred)
                report("配置中的设备均不可用或无线耳机未连接，保留当前默认设备，等待设备连接。");
            else
            {
                const auto& device = devices[*preferred];
                const bool alreadyDefault = device.isDefaultConsole && device.isDefaultMultimedia
                    && (!config.includeCommunications || device.isDefaultCommunications);
                if (dryRun)
                    report("优先设备：" + ToUtf8(device.name) + (alreadyDefault ? "（已是默认设备）" : "（将切换到此设备）"));
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
                    report("当前优先设备：" + ToUtf8(device.name));
                }
            }
        }
        catch (const std::exception& error)
        {
            if (once) throw;
            report(std::string("音频操作失败，稍后重试：") + error.what());
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
    std::cout << "监听已停止。\n";
}
