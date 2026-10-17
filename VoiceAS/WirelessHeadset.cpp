#include "WirelessHeadset.h"
#include "WindowsSupport.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <devpkey.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <cwctype>
#include <utility>

#pragma comment(lib, "Hid.lib")
#pragma comment(lib, "Setupapi.lib")

namespace
{
    using namespace windows_support;
    struct DeviceInfoDeleter
    {
        void operator()(void* value) const noexcept { SetupDiDestroyDeviceInfoList(value); }
    };
    struct PreparsedData
    {
        PHIDP_PREPARSED_DATA value = nullptr;
        ~PreparsedData() { if (value) HidD_FreePreparsedData(value); }
    };

    std::string Hex(std::span<const unsigned char> bytes)
    {
        std::ostringstream text;
        for (size_t index = 0; index < bytes.size() && index < 16; ++index)
            text << (index ? " " : "") << std::hex << std::uppercase
                << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[index]);
        return text.str();
    }

    std::wstring ContainerId(HDEVINFO info, SP_DEVINFO_DATA& device)
    {
        GUID container{};
        DEVPROPTYPE type = 0;
        if (!SetupDiGetDevicePropertyW(info, &device, &DEVPKEY_Device_ContainerId, &type,
            reinterpret_cast<PBYTE>(&container), sizeof(container), nullptr, 0) || type != DEVPROP_TYPE_GUID)
            return {};
        if (IsEqualGUID(container, GUID_NULL)) return {};
        wchar_t text[39]{};
        return StringFromGUID2(container, text, 39) > 0 ? text : L"";
    }

    DWORD Transfer(HANDLE device, bool write, unsigned char* bytes, DWORD size, DWORD timeout)
    {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) throw std::runtime_error("Cannot create HID I/O event.");
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD transferred = 0;
        const BOOL completed = write ? WriteFile(device, bytes, size, &transferred, &operation)
            : ReadFile(device, bytes, size, &transferred, &operation);
        if (!completed)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
                ThrowWin32Error("HID I/O", error);
            if (WaitForSingleObject(event.get(), timeout) != WAIT_OBJECT_0)
            {
                CancelIoEx(device, &operation);
                // The OVERLAPPED and buffer must remain alive until cancellation completes.
                GetOverlappedResult(device, &operation, &transferred, TRUE);
                throw std::runtime_error("HID status query timed out.");
            }
            if (!GetOverlappedResult(device, &operation, &transferred, FALSE))
                ThrowWin32Error("HID completion");
        }
        return transferred;
    }

    WirelessHeadsetStatus QueryDevice(HANDLE device, const HIDP_CAPS& caps)
    {
        // Protocol facts verified against G-Helper's AsusHeadset.ReadBattery:
        // https://github.com/seerge/g-helper/blob/main/app/Peripherals/Headset/AsusHeadset.cs
        // 0x12 = GET, 0x07 = battery/readiness; no SET command is sent.
        std::vector<unsigned char> request(caps.OutputReportByteLength, 0);
        request[0] = 0xCC;
        request[1] = 0x12;
        request[2] = 0x07;
        if (Transfer(device, true, request.data(), static_cast<DWORD>(request.size()), 800) != request.size())
            throw std::runtime_error("Incomplete HID status request.");
        const ULONGLONG deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline)
        {
            std::vector<unsigned char> reply(caps.InputReportByteLength, 0);
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            const auto received = Transfer(device, false, reply.data(), static_cast<DWORD>(reply.size()),
                static_cast<DWORD>(deadline - now));
            reply.resize(received);
            const auto decoded = DecodeRogStatus(reply);
            if (decoded.state != WirelessConnectionState::Unknown) return decoded;
        }
        throw std::runtime_error("No matching ROG status response.");
    }
}

WirelessHeadsetStatus DecodeRogStatus(std::span<const unsigned char> response)
{
    WirelessHeadsetStatus result;
    result.detail = Hex(response);
    if (response.size() < 9 || response[0] != 0xCC) return result;
    if ((response[1] == 0xFF && response[2] == 0xAA)
        || (response[1] == 0x12 && response[2] == 0x07 && response[5] == 0xFF && response[6] == 0xAA))
        result.state = WirelessConnectionState::Offline;
    else if (response[1] == 0x12 && response[2] == 0x07 && response[3] == 0 && response[4] == 0)
    {
        if (response[6] <= 100)
        {
            result.state = WirelessConnectionState::Online;
            result.batteryPercent = response[6];
        }
    }
    return result;
}

std::vector<WirelessHeadsetStatus> QueryWirelessHeadsets()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    const auto rawInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (rawInfo == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate HID interfaces.");
    const std::unique_ptr<void, DeviceInfoDeleter> info(rawInfo);
    std::vector<WirelessHeadsetStatus> statuses;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(info.get(), nullptr, &hidGuid, index, &interfaceData))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw std::runtime_error("Failed to enumerate HID interface.");
        }
        DWORD bytesNeeded = 0;
        SetupDiGetDeviceInterfaceDetailW(info.get(), &interfaceData, nullptr, 0, &bytesNeeded, nullptr);
        if (bytesNeeded < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<unsigned char> storage(bytesNeeded);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(info.get(), &interfaceData, detail, bytesNeeded, nullptr, &deviceInfo)) continue;
        std::wstring path = detail->DevicePath;
        for (auto& letter : path) letter = static_cast<wchar_t>(towlower(letter));
        if (path.find(L"vid_0b05&pid_1afa") == std::wstring::npos) continue;

        WirelessHeadsetStatus status;
        status.containerId = ContainerId(info.get(), deviceInfo);
        const auto rawDevice = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (rawDevice == INVALID_HANDLE_VALUE)
        {
            status.detail = "Cannot open ROG HID interface (Windows error " + std::to_string(GetLastError()) + ").";
            statuses.push_back(std::move(status));
            continue;
        }
        const UniqueHandle device(rawDevice);
        PreparsedData preparsed;
        HIDP_CAPS caps{};
        if (!HidD_GetPreparsedData(device.get(), &preparsed.value)
            || HidP_GetCaps(preparsed.value, &caps) != HIDP_STATUS_SUCCESS) continue;
        if (caps.UsagePage != 0xFF00 || caps.OutputReportByteLength < 64 || caps.InputReportByteLength < 7) continue;
        try { status = QueryDevice(device.get(), caps); }
        catch (const std::exception& error) { status.detail = error.what(); }
        status.containerId = ContainerId(info.get(), deviceInfo);
        statuses.push_back(std::move(status));
    }
    return statuses;
}

void UpdateWirelessAvailability(std::vector<AudioOutputDevice>& devices)
{
    const auto statuses = QueryWirelessHeadsets();
    for (auto& device : devices)
    {
        if (device.containerId.empty()) continue;
        for (const auto& status : statuses)
        {
            if (device.containerId != status.containerId) continue;
            // A successful control interface takes precedence over inaccessible consumer interfaces.
            if (device.wirelessState == WirelessConnectionState::Online || device.wirelessState == WirelessConnectionState::Offline)
                continue;
            device.wirelessState = status.state;
            device.batteryPercent = status.batteryPercent;
            device.wirelessDetail = status.detail;
        }
    }
}

const char* WirelessStateName(WirelessConnectionState state)
{
    switch (state)
    {
    case WirelessConnectionState::Online: return "Online";
    case WirelessConnectionState::Offline: return "Offline";
    case WirelessConnectionState::Unknown: return "Unknown";
    default: return "Not applicable";
    }
}
