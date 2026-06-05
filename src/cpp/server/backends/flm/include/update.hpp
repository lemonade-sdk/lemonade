/*
*  Copyright (c) 2025 by Contributors
*  \file update.hpp
*  \brief Detect new version
*  \author FastFlowLM Team
*  \date 2025-09-24
*  \version 0.9.21
*/

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

#include <optional>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>
#include <cstdlib>

#include "utils/utils.hpp"


static std::string strip_v_prefix(std::string s) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
    return s;
}

static std::vector<int> split_semver(const std::string& v) {
    std::vector<int> out; out.reserve(3);
    size_t start = 0;
    while (start < v.size()) {
        size_t dot = v.find('.', start);
        std::string part = v.substr(start, (dot == std::string::npos) ? std::string::npos : (dot - start));
        size_t i = 0; while (i < part.size() && std::isdigit(static_cast<unsigned char>(part[i]))) ++i;
        int n = 0; if (i) n = std::stoi(part.substr(0, i));
        out.push_back(n);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

static int compare_semver(std::string a, std::string b) {
    a = strip_v_prefix(a);
    b = strip_v_prefix(b);
    auto A = split_semver(a), B = split_semver(b);
    for (int i = 0; i < 3; ++i) {
        if (A[i] != B[i]) return (A[i] > B[i]) ? 1 : -1;
    }
    return 0;
}

static bool g_vercheck_warned_timeout = false;

static bool version_check_disabled() {
    const char* env = std::getenv("FLM_DISABLE_UPDATE_CHECK");
    if (!env) return false;

    std::string value(env);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

#ifdef _WIN32

static bool is_timeout_error(DWORD err) {
    return err == ERROR_WINHTTP_TIMEOUT                  // 12002
        || err == ERROR_WINHTTP_CANNOT_CONNECT                  // sometimes surfaces with timeouts
        || err == ERROR_WINHTTP_CONNECTION_ERROR         // 12030 (often due to handshake timeout)
        || err == ERROR_WINHTTP_NAME_NOT_RESOLVED        // DNS timeout-like
        || err == ERROR_WINHTTP_CANNOT_CONNECT          // connect timeout-like
        || err == WAIT_TIMEOUT;
}

static std::string fetch_path_with_timeout_flag(HINTERNET hSession,
    const wchar_t* host,
    INTERNET_PORT port,
    const std::wstring& path,
    bool& out_timed_out)
{
    out_timed_out = false;
    std::string body;

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) {
        DWORD e = GetLastError();
        if (is_timeout_error(e)) out_timed_out = true;
        return body;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        DWORD e = GetLastError();
        if (is_timeout_error(e)) out_timed_out = true;
        WinHttpCloseHandle(hConnect);
        return body;
    }

    WinHttpAddRequestHeaders(hRequest, L"User-Agent: flm/1.0\r\n", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        DWORD e = GetLastError();
        if (is_timeout_error(e)) out_timed_out = true;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return body;
    }

    ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!ok) {
        DWORD e = GetLastError();
        if (is_timeout_error(e)) out_timed_out = true;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return body;
    }

    for (;;) {
        DWORD dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            DWORD e = GetLastError();
            if (is_timeout_error(e)) out_timed_out = true;
            break;
        }
        if (dwSize == 0) break;
        std::string buf; buf.resize(dwSize);
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
            DWORD e = GetLastError();
            if (is_timeout_error(e)) out_timed_out = true;
            break;
        }
        buf.resize(dwDownloaded);
        body += buf;
        if (dwDownloaded == 0) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return body;
}



// FastFlowLM/FastFlowLM
static std::optional<std::string> http_get_latest_tag_from_github(bool& timed_out) {
    timed_out = false;

    const wchar_t* host = L"api.github.com";
    const INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;

    HINTERNET hSession = WinHttpOpen(L"FLM-Version-Check/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { timed_out = is_timeout_error(GetLastError()); return std::nullopt; }

    DWORD timeout = 800; // ms
    WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

    // 1) releases/latest
    {
        bool t = false;
        std::string body = fetch_path_with_timeout_flag(hSession, host, port,
            L"/repos/FastFlowLM/FastFlowLM/releases/latest", t);
        timed_out = timed_out || t;
        if (!body.empty()) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.contains("tag_name") && j["tag_name"].is_string()) {
                    WinHttpCloseHandle(hSession);
                    return j["tag_name"].get<std::string>();
                }
            }
            catch (...) { /* fall through */ }
        }
    }

    // 2) fallback: tags?per_page=1
    {
        bool t = false;
        std::string body = fetch_path_with_timeout_flag(hSession, host, port,
            L"/repos/FastFlowLM/FastFlowLM/tags?per_page=1", t);
        timed_out = timed_out || t;
        if (!body.empty()) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.is_array() && !j.empty() && j[0].contains("name") && j[0]["name"].is_string()) {
                    WinHttpCloseHandle(hSession);
                    return j[0]["name"].get<std::string>();
                }
            }
            catch (...) { /* ignore */ }
        }
    }

    WinHttpCloseHandle(hSession);
    return std::nullopt;
}


static void check_and_notify_new_version() {
    if (version_check_disabled()) return;

    bool timed_out = false;
    auto latest_opt = http_get_latest_tag_from_github(timed_out);

    if (timed_out && !g_vercheck_warned_timeout) {
        g_vercheck_warned_timeout = true;
        header_print("Warning", "Version check timed out; continuing without update info.");
        // std::cout << "[FLM] Warning: version check timed out; continuing without update info.\n";
    }

    if (!latest_opt.has_value()) return;

    const std::string current = __FLM_VERSION__;
    const std::string latest = latest_opt.value();
    if (compare_semver(latest, current) > 0) {
        header_print("FLM", "New version detected! (current v" << current << ", latest " << latest << ")");
        header_print("FLM", "Download link: https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe");
    }
}
#else
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// FastFlowLM/FastFlowLM (non-Windows)
static std::optional<std::string> http_get_latest_tag_from_github(bool& timed_out) {
    timed_out = false;
    auto fetch_url = [&](const char* url) -> std::optional<std::string> {
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "flm/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 800L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 800L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OPERATION_TIMEDOUT) {
            timed_out = true;
        }
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || body.empty()) {
            return std::nullopt;
        }
        return body;
    };

    if (auto body = fetch_url("https://api.github.com/repos/FastFlowLM/FastFlowLM/releases/latest")) {
        try {
            auto j = nlohmann::json::parse(*body);
            if (j.contains("tag_name") && j["tag_name"].is_string()) {
                return j["tag_name"].get<std::string>();
            }
        } catch (...) {}
    }

    if (auto body = fetch_url("https://api.github.com/repos/FastFlowLM/FastFlowLM/tags?per_page=1")) {
        try {
            auto j = nlohmann::json::parse(*body);
            if (j.is_array() && !j.empty() && j[0].contains("name") && j[0]["name"].is_string()) {
                return j[0]["name"].get<std::string>();
            }
        } catch (...) {}
    }

    return std::nullopt;
}

static void check_and_notify_new_version() {
    if (version_check_disabled()) return;

    bool timed_out = false;
    auto latest_opt = http_get_latest_tag_from_github(timed_out);

    if (timed_out && !g_vercheck_warned_timeout) {
        g_vercheck_warned_timeout = true;
        header_print("Warning", "Version check timed out; continuing without update info.");
    }

    if (!latest_opt.has_value()) return;

    const std::string current = __FLM_VERSION__;
    const std::string latest = latest_opt.value();
    if (compare_semver(latest, current) > 0) {
        header_print("FLM", "New version detected! (current v" << current << ", latest " << latest << ")");
        header_print("FLM", "Download link: https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe");
    }
}
#endif
