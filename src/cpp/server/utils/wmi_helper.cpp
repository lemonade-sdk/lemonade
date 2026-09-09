#ifdef _WIN32

#include "wmi_helper.h"
#include <lemon/utils/aixlog.hpp>
#include <iostream>
#include <locale>
#include <codecvt>
#include <algorithm>
// INITGUID must be defined before devpkey.h in exactly one .cpp to emit DEVPKEY symbols
#include <initguid.h>
#include <devpkey.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "setupapi.lib")

namespace lemon {
namespace wmi {

// ============================================================================
// WMIConnection implementation
// ============================================================================

WMIConnection::WMIConnection() {
    // Initialize COM
    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres) && hres != RPC_E_CHANGED_MODE) {
        return;
    }

    // Initialize COM security
    hres = CoInitializeSecurity(
        NULL,
        -1,
        NULL,
        NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE,
        NULL
    );

    // Create WMI locator
    hres = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        (LPVOID*)&pLoc_
    );

    if (FAILED(hres)) {
        return;
    }

    // Connect to WMI
    hres = pLoc_->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"),
        NULL,
        NULL,
        0,
        NULL,
        0,
        0,
        &pSvc_
    );

    if (FAILED(hres)) {
        pLoc_->Release();
        pLoc_ = nullptr;
        return;
    }

    // Set security levels on proxy
    hres = CoSetProxyBlanket(
        pSvc_,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE
    );

    if (FAILED(hres)) {
        pSvc_->Release();
        pSvc_ = nullptr;
        pLoc_->Release();
        pLoc_ = nullptr;
    }
}

WMIConnection::~WMIConnection() {
    if (pSvc_) {
        pSvc_->Release();
    }
    if (pLoc_) {
        pLoc_->Release();
    }
}

bool WMIConnection::query(const std::wstring& wql_query,
                          std::function<void(IWbemClassObject*)> callback) {
    if (!is_valid()) {
        return false;
    }

    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc_->ExecQuery(
        bstr_t("WQL"),
        bstr_t(wql_query.c_str()),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL,
        &pEnumerator
    );

    if (FAILED(hres)) {
        return false;
    }

    // Iterate through results
    IWbemClassObject* pclsObj = nullptr;
    ULONG uReturn = 0;

    while (pEnumerator) {
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);

        if (0 == uReturn) {
            break;
        }

        callback(pclsObj);
        pclsObj->Release();
    }

    pEnumerator->Release();
    return true;
}

// ============================================================================
// Helper functions
// ============================================================================

std::wstring string_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::string acp_to_utf8(const std::string& acp_str) {
    if (acp_str.empty()) return std::string();

    // ACP (system locale code page) -> Wide string
    int wide_size = MultiByteToWideChar(CP_ACP, 0, acp_str.c_str(), (int)acp_str.size(), NULL, 0);
    if (wide_size == 0) return acp_str;
    std::wstring wstr(wide_size, 0);
    MultiByteToWideChar(CP_ACP, 0, acp_str.c_str(), (int)acp_str.size(), &wstr[0], wide_size);

    // Wide string -> UTF-8
    return wstring_to_string(wstr);
}

std::string get_property_string(IWbemClassObject* pObj, const std::wstring& prop_name) {
    VARIANT vtProp;
    VariantInit(&vtProp);

    HRESULT hr = pObj->Get(prop_name.c_str(), 0, &vtProp, 0, 0);
    if (FAILED(hr) || vtProp.vt != VT_BSTR) {
        VariantClear(&vtProp);
        return "";
    }

    std::wstring wstr(vtProp.bstrVal, SysStringLen(vtProp.bstrVal));
    std::string result = wstring_to_string(wstr);

    VariantClear(&vtProp);
    return result;
}

int get_property_int(IWbemClassObject* pObj, const std::wstring& prop_name) {
    VARIANT vtProp;
    VariantInit(&vtProp);

    HRESULT hr = pObj->Get(prop_name.c_str(), 0, &vtProp, 0, 0);
    if (FAILED(hr)) {
        VariantClear(&vtProp);
        return 0;
    }

    int result = 0;
    if (vtProp.vt == VT_I4) {
        result = vtProp.lVal;
    } else if (vtProp.vt == VT_UI4) {
        result = vtProp.ulVal;
    }

    VariantClear(&vtProp);
    return result;
}

uint64_t get_property_uint64(IWbemClassObject* pObj, const std::wstring& prop_name) {
    VARIANT vtProp;
    VariantInit(&vtProp);

    HRESULT hr = pObj->Get(prop_name.c_str(), 0, &vtProp, 0, 0);
    if (FAILED(hr)) {
        VariantClear(&vtProp);
        return 0;
    }

    uint64_t result = 0;
    try {
        if (vtProp.vt == VT_BSTR) {
            // Sometimes returned as string - parse safely
            if (vtProp.bstrVal != nullptr) {
                std::wstring wstr(vtProp.bstrVal);
                if (!wstr.empty()) {
                    result = std::stoull(wstr);
                }
            }
        } else if (vtProp.vt == VT_UI8) {
            result = vtProp.ullVal;
        } else if (vtProp.vt == VT_UI4) {
            result = vtProp.ulVal;
        } else if (vtProp.vt == VT_I4) {
            result = static_cast<uint64_t>(vtProp.lVal);
        }
    } catch (const std::exception& e) {
        // Parsing failed - return 0 instead of crashing
        LOG(WARNING, "WMI") << "Failed to parse uint64 property: " << e.what() << std::endl;
        result = 0;
    }

    VariantClear(&vtProp);
    return result;
}

namespace {

// Walks every present device and hands the first whose friendly name (or, when
// that is empty, its device description) contains `device_name_substr` to
// `visit`. Matching is case-insensitive; enumeration stops when `visit` returns
// true.
void with_matching_device(const std::string& device_name_substr,
                          const std::function<bool(HDEVINFO, SP_DEVINFO_DATA&)>& visit) {
    std::string needle = device_name_substr;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    HDEVINFO dev_info = SetupDiGetClassDevs(NULL, NULL, NULL,
                                            DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
        return;
    }

    SP_DEVINFO_DATA dev_data = {};
    dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data); ++i) {
        // Always use the description if the friendly name is empty, which is
        // common for software components.
        wchar_t name_buf[512] = {};
        bool got_name = SetupDiGetDeviceRegistryPropertyW(dev_info, &dev_data,
                                                          SPDRP_FRIENDLYNAME, NULL,
                                                          (PBYTE)name_buf, sizeof(name_buf),
                                                          NULL)
                        && name_buf[0] != L'\0';
        if (!got_name) {
            if (!SetupDiGetDeviceRegistryPropertyW(dev_info, &dev_data,
                                                   SPDRP_DEVICEDESC, NULL,
                                                   (PBYTE)name_buf, sizeof(name_buf),
                                                   NULL)) {
                continue;
            }
        }

        std::string name_lower = wstring_to_string(name_buf);
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        if (name_lower.find(needle) == std::string::npos) {
            continue;
        }

        if (visit(dev_info, dev_data)) {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
}

std::string read_reg_string(HKEY hkey, const wchar_t* value_name) {
    wchar_t buf[512] = {};
    // Reserve the final element so a value that fills the buffer stays terminated.
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(hkey, value_name, NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
        return wstring_to_string(buf);
    }
    return "";
}

// Opens HKLM\SYSTEM\CurrentControlSet\Control\Class\<SPDRP_DRIVER>, the key
// holding the vendor's own per-adapter driver metadata. Returns NULL on failure.
HKEY open_class_driver_key(HDEVINFO dev_info, SP_DEVINFO_DATA& dev_data) {
    wchar_t driver_key[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(dev_info, &dev_data, SPDRP_DRIVER, NULL,
                                           (PBYTE)driver_key, sizeof(driver_key), NULL)) {
        return NULL;
    }

    std::wstring reg_path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
    reg_path += driver_key;

    HKEY hkey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_path.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return NULL;
    }
    return hkey;
}

} // namespace

std::string get_driver_version_setupapi(const std::string& device_name_substr) {
    std::string result;

    with_matching_device(device_name_substr, [&result](HDEVINFO dev_info, SP_DEVINFO_DATA& dev_data) {
        // First try DEVPKEY_Device_DriverVersion (works for most hardware devices).
        DEVPROPTYPE prop_type = 0;
        wchar_t ver_buf[256] = {};
        DWORD required = 0;
        if (SetupDiGetDevicePropertyW(dev_info, &dev_data,
                                      &DEVPKEY_Device_DriverVersion,
                                      &prop_type,
                                      (PBYTE)ver_buf, sizeof(ver_buf),
                                      &required, 0)) {
            result = wstring_to_string(ver_buf);
        }

        // Fallback: for SOFTWARECOMPONENT devices DEVPKEY_Device_DriverVersion is
        // not populated. Open the device's registry key directly and read DriverVersion.
        if (result.empty()) {
            HKEY hkey = SetupDiOpenDevRegKey(dev_info, &dev_data,
                                             DICS_FLAG_GLOBAL, 0,
                                             DIREG_DEV, KEY_READ);
            if (hkey != INVALID_HANDLE_VALUE) {
                result = read_reg_string(hkey, L"DriverVersion");
                RegCloseKey(hkey);
            }
        }

        // Second fallback: the class driver key.
        if (result.empty()) {
            HKEY hkey = open_class_driver_key(dev_info, dev_data);
            if (hkey) {
                result = read_reg_string(hkey, L"DriverVersion");
                RegCloseKey(hkey);
            }
        }
        return true;
    });

    return result;
}

std::string get_driver_key_value_setupapi(const std::string& device_name_substr,
                                          const std::wstring& value_name) {
    std::string result;

    with_matching_device(device_name_substr, [&result, &value_name](HDEVINFO dev_info,
                                                                    SP_DEVINFO_DATA& dev_data) {
        HKEY hkey = open_class_driver_key(dev_info, dev_data);
        if (hkey) {
            result = read_reg_string(hkey, value_name.c_str());
            RegCloseKey(hkey);
        }
        // Keep looking when an identically named adapter matched first but
        // carried no value.
        return !result.empty();
    });

    return result;
}

} // namespace wmi
} // namespace lemon

#endif // _WIN32
