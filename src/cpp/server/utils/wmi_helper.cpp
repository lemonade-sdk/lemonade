#ifdef _WIN32

#include "wmi_helper.h"
#include <iostream>
#include <locale>
#include <codecvt>

#pragma comment(lib, "wbemuuid.lib")

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
    if (vtProp.vt == VT_BSTR) {
        // Sometimes returned as string
        std::wstring wstr(vtProp.bstrVal);
        result = std::stoull(wstr);
    } else if (vtProp.vt == VT_UI8) {
        result = vtProp.ullVal;
    }
    
    VariantClear(&vtProp);
    return result;
}

} // namespace wmi
} // namespace lemon

#endif // _WIN32

