#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <exception>
#include <iostream>

#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>

#if defined(_MSC_VER)
#pragma comment(lib, "wbemuuid.lib")
#endif

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
    IWbemLocator*  pLoc_ = nullptr;
    IWbemServices* pSvc_ = nullptr;

    // Track whether we successfully called CoInitializeEx() on this thread.
    // If CoInitializeEx returns RPC_E_CHANGED_MODE, we did not initialize COM and must not uninitialize.
    HRESULT coinit_hr_ = E_FAIL;
};

// Helper functions
std::wstring string_to_wstring(const std::string& str);
std::string  wstring_to_string(const std::wstring& wstr);
std::string  get_property_string(IWbemClassObject* pObj, const std::wstring& prop_name);
int          get_property_int(IWbemClassObject* pObj, const std::wstring& prop_name);
uint64_t     get_property_uint64(IWbemClassObject* pObj, const std::wstring& prop_name);

// ============================================================================
// Inline implementation
// ============================================================================

inline WMIConnection::WMIConnection() {
    // Initialize COM
    coinit_hr_ = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(coinit_hr_) && coinit_hr_ != RPC_E_CHANGED_MODE) {
        return;
    }

    // Initialize COM security (may legitimately fail with RPC_E_TOO_LATE if already set)
    HRESULT hres = CoInitializeSecurity(
        NULL,
        -1,
        NULL,
        NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE,
        NULL);
    (void)hres;

    // Create WMI locator
    hres = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        (LPVOID*)&pLoc_);

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
        &pSvc_);

    if (FAILED(hres)) {
        pLoc_->Release();
        pLoc_ = nullptr;
        return;
    }

    // Set security levels on the proxy
    hres = CoSetProxyBlanket(
        pSvc_,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE);

    if (FAILED(hres)) {
        pSvc_->Release();
        pSvc_ = nullptr;
        pLoc_->Release();
        pLoc_ = nullptr;
        return;
    }
}

inline WMIConnection::~WMIConnection() {
    if (pSvc_) {
        pSvc_->Release();
        pSvc_ = nullptr;
    }
    if (pLoc_) {
        pLoc_->Release();
        pLoc_ = nullptr;
    }

    if (SUCCEEDED(coinit_hr_)) {
        CoUninitialize();
    }
}

inline bool WMIConnection::query(const std::wstring& wql_query,
                                std::function<void(IWbemClassObject*)> callback) {
    if (!is_valid()) {
        return false;
    }

    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc_->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(wql_query.c_str()),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL,
        &pEnumerator);

    if (FAILED(hres) || !pEnumerator) {
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
        pclsObj = nullptr;
    }

    pEnumerator->Release();
    return true;
}

inline std::wstring string_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

inline std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

inline std::string get_property_string(IWbemClassObject* pObj, const std::wstring& prop_name) {
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

inline int get_property_int(IWbemClassObject* pObj, const std::wstring& prop_name) {
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
        result = static_cast<int>(vtProp.ulVal);
    }

    VariantClear(&vtProp);
    return result;
}

inline uint64_t get_property_uint64(IWbemClassObject* pObj, const std::wstring& prop_name) {
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
            result = static_cast<uint64_t>(vtProp.ullVal);
        } else if (vtProp.vt == VT_UI4) {
            result = static_cast<uint64_t>(vtProp.ulVal);
        } else if (vtProp.vt == VT_I4) {
            result = static_cast<uint64_t>(vtProp.lVal);
        }
    } catch (const std::exception& e) {
        // Parsing failed - return 0 instead of crashing
        std::cerr << "[WMI WARNING] Failed to parse uint64 property: " << e.what() << std::endl;
        result = 0;
    }

    VariantClear(&vtProp);
    return result;
}

} // namespace wmi

#endif // _WIN32
