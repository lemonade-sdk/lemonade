#pragma once

#ifdef _WIN32

#include <string>
#include <vector>
#include <functional>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>

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
std::string get_property_string(IWbemClassObject* pObj, const std::wstring& prop_name);
int get_property_int(IWbemClassObject* pObj, const std::wstring& prop_name);
uint64_t get_property_uint64(IWbemClassObject* pObj, const std::wstring& prop_name);

} // namespace wmi
} // namespace lemon

#endif // _WIN32
