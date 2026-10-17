#pragma once

#include <Windows.h>
#include <objbase.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace windows_support
{
    inline void CheckResult(HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            std::ostringstream message;
            message << operation << " failed (HRESULT 0x"
                << std::hex << std::uppercase << std::setw(8)
                << std::setfill('0') << static_cast<unsigned long>(result) << ").";
            throw std::runtime_error(message.str());
        }
    }

    class ComApartment
    {
    public:
        ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
        {
            if (result_ != RPC_E_CHANGED_MODE)
            {
                CheckResult(result_, "CoInitializeEx");
            }
        }
        ~ComApartment()
        {
            if (SUCCEEDED(result_)) CoUninitialize();
        }
        ComApartment(const ComApartment&) = delete;
        ComApartment& operator=(const ComApartment&) = delete;
    private:
        HRESULT result_;
    };

    struct CoTaskMemDeleter
    {
        void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
    };

    struct HandleDeleter
    {
        void operator()(void* value) const noexcept { if (value) CloseHandle(value); }
    };
    using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

    inline std::string ToUtf8(std::wstring_view value)
    {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size == 0) throw std::runtime_error("Invalid Unicode text.");
        std::string result(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0)
            throw std::runtime_error("Unable to encode UTF-8 text.");
        return result;
    }

    inline std::wstring FromUtf8(std::string_view value)
    {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size == 0) throw std::runtime_error("Invalid UTF-8 text in configuration.");
        std::wstring result(size, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size) == 0)
            throw std::runtime_error("Unable to decode UTF-8 text.");
        return result;
    }
}
