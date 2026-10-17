#include "EnumerateVoiceDevices.h"
#include "WindowsSupport.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include <memory>
#include <utility>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(lib, "Cfgmgr32.lib")

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace windows_support;

    std::wstring GetContainerId(const std::wstring& endpointId)
    {
        std::wstring instanceId = L"SWD\\MMDEVAPI\\" + endpointId;
        DEVINST deviceNode = 0;
        if (CM_Locate_DevNodeW(&deviceNode, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            return {};
        GUID container{};
        DEVPROPTYPE type = 0;
        ULONG size = sizeof(container);
        if (CM_Get_DevNode_PropertyW(deviceNode, &DEVPKEY_Device_ContainerId, &type,
            reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID)
            return {};
        if (IsEqualGUID(container, GUID_NULL)) return {};
        wchar_t text[39]{};
        return StringFromGUID2(container, text, 39) > 0 ? text : L"";
    }

    std::wstring GetDeviceId(IMMDevice* device)
    {
        wchar_t* rawId = nullptr;
        const HRESULT result = device->GetId(&rawId);
        const std::unique_ptr<wchar_t, CoTaskMemDeleter> id(rawId);
        CheckResult(result, "IMMDevice::GetId");
        return id.get();
    }

    std::wstring GetDefaultDeviceId(IMMDeviceEnumerator* enumerator, ERole role)
    {
        ComPtr<IMMDevice> device;
        const HRESULT result = enumerator->GetDefaultAudioEndpoint(
            eRender, role, device.GetAddressOf());
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            return {}; // No default is normal when no output device is available.
        }

        CheckResult(result, "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
        return GetDeviceId(device.Get());
    }

    std::wstring GetDeviceName(IMMDevice* device)
    {
        ComPtr<IPropertyStore> properties;
        if (FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf())))
        {
            return L"(name unavailable)";
        }

        struct ScopedProperty
        {
            PROPVARIANT value;
            ScopedProperty() { PropVariantInit(&value); }
            ~ScopedProperty() { PropVariantClear(&value); }
            ScopedProperty(const ScopedProperty&) = delete;
            ScopedProperty& operator=(const ScopedProperty&) = delete;
        } name;

        const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &name.value);
        if (SUCCEEDED(result) && name.value.vt == VT_LPWSTR
            && name.value.pwszVal != nullptr && name.value.pwszVal[0] != L'\0')
        {
            return name.value.pwszVal;
        }

        // Keep the endpoint in the list even if its friendly name is unavailable.
        return L"(name unavailable)";
    }

    AudioDeviceState GetDeviceState(DWORD state)
    {
        switch (state)
        {
        case DEVICE_STATE_ACTIVE: return AudioDeviceState::Active;
        case DEVICE_STATE_DISABLED: return AudioDeviceState::Disabled;
        case DEVICE_STATE_NOTPRESENT: return AudioDeviceState::NotPresent;
        case DEVICE_STATE_UNPLUGGED: return AudioDeviceState::Unplugged;
        default: return AudioDeviceState::Unknown;
        }
    }
}

std::vector<AudioOutputDevice> EnumerateAudioOutputDevices()
{
    const ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    CheckResult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf())), "CoCreateInstance");

    const auto defaultConsole = GetDefaultDeviceId(enumerator.Get(), eConsole);
    const auto defaultMultimedia = GetDefaultDeviceId(enumerator.Get(), eMultimedia);
    const auto defaultCommunications = GetDefaultDeviceId(enumerator.Get(), eCommunications);

    ComPtr<IMMDeviceCollection> collection;
    CheckResult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL,
        collection.GetAddressOf()), "IMMDeviceEnumerator::EnumAudioEndpoints");

    UINT count = 0;
    CheckResult(collection->GetCount(&count), "IMMDeviceCollection::GetCount");

    std::vector<AudioOutputDevice> devices;
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
        ComPtr<IMMDevice> endpoint;
        CheckResult(collection->Item(index, endpoint.GetAddressOf()), "IMMDeviceCollection::Item");

        AudioOutputDevice device;
        device.id = GetDeviceId(endpoint.Get());
        device.containerId = GetContainerId(device.id);
        device.name = GetDeviceName(endpoint.Get());

        DWORD state = 0;
        CheckResult(endpoint->GetState(&state), "IMMDevice::GetState");
        device.state = GetDeviceState(state);
        device.isDefaultConsole = !defaultConsole.empty() && device.id == defaultConsole;
        device.isDefaultMultimedia = !defaultMultimedia.empty() && device.id == defaultMultimedia;
        device.isDefaultCommunications = !defaultCommunications.empty() && device.id == defaultCommunications;
        devices.push_back(std::move(device));
    }

    return devices;
}
