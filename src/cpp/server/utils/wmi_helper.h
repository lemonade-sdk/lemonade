#pragma once

#ifdef _WIN32

#include <string>
#include <vector>
#include <functional>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <setupapi.h>

namespace lemon {
namespace wmi {

// RAII wrapper for COM initialization
class COMInitializer {
public:
    COMInitializer() {
        hr_ = CoInitializeEx(0, COINIT_MULTITHREADED);
    }

    ~COMInitializer() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    bool succeeded() const { return SUCCEEDED(hr_); }

private:
    HRESULT hr_;
};

// RAII wrapper for WMI connection
class WMIConnection {
public:
    WMIConnection();
    ~WMIConnection();

    bool is_valid() const { return pSvc_ != nullptr; }

    // Query WMI and call callback for each result
    bool query(const std::wstring& wql_query,
               std::function<void(IWbemClassObject*)> callback);

private:
    IWbemLocator* pLoc_ = nullptr;
    IWbemServices* pSvc_ = nullptr;
};

// Helper functions
std::wstring string_to_wstring(const std::string& str);
std::string wstring_to_string(const std::wstring& wstr);
std::string acp_to_utf8(const std::string& acp_str);
std::string get_property_string(IWbemClassObject* pObj, const std::wstring& prop_name);
int get_property_int(IWbemClassObject* pObj, const std::wstring& prop_name);
uint64_t get_property_uint64(IWbemClassObject* pObj, const std::wstring& prop_name);

// SetupAPI-based driver version lookup.
// Enumerates all present devices via SetupAPI and returns the driver version
// string for the first device whose friendly name contains device_name_substr
// (case-insensitive). Returns empty string if not found.
// Much faster than Win32_PnPSignedDriver WMI queries (~5-50 ms vs ~10 s).
std::string get_driver_version_setupapi(const std::string& device_name_substr);

} // namespace wmi
} // namespace lemon

#endif // _WIN32
