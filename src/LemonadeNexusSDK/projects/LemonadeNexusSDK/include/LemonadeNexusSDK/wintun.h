#pragma once

// WinTun API — runtime-loaded from wintun.dll
// See: https://www.wintun.net/ and https://git.zx2c4.com/wintun/about/

#ifdef _WIN32

#include <windows.h>
#include <iphlpapi.h>

// Opaque handles
typedef void* WINTUN_ADAPTER_HANDLE;
typedef void* WINTUN_SESSION_HANDLE;

// Function pointer types matching the WinTun DLL exports
typedef WINTUN_ADAPTER_HANDLE (WINAPI *WINTUN_CREATE_ADAPTER_FUNC)(
    LPCWSTR Name, LPCWSTR TunnelType, const GUID* RequestedGUID);

typedef void (WINAPI *WINTUN_CLOSE_ADAPTER_FUNC)(
    WINTUN_ADAPTER_HANDLE Adapter);

typedef WINTUN_SESSION_HANDLE (WINAPI *WINTUN_START_SESSION_FUNC)(
    WINTUN_ADAPTER_HANDLE Adapter, DWORD Capacity);

typedef void (WINAPI *WINTUN_END_SESSION_FUNC)(
    WINTUN_SESSION_HANDLE Session);

typedef HANDLE (WINAPI *WINTUN_GET_READ_WAIT_EVENT_FUNC)(
    WINTUN_SESSION_HANDLE Session);

typedef BYTE* (WINAPI *WINTUN_RECEIVE_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, DWORD* PacketSize);

typedef void (WINAPI *WINTUN_RELEASE_RECEIVE_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, const BYTE* Packet);

typedef BYTE* (WINAPI *WINTUN_ALLOCATE_SEND_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, DWORD PacketSize);

typedef void (WINAPI *WINTUN_SEND_PACKET_FUNC)(
    WINTUN_SESSION_HANDLE Session, const BYTE* Packet);

/// Holds all WinTun function pointers loaded at runtime.
struct WintunApi {
    HMODULE                             dll{nullptr};
    WINTUN_CREATE_ADAPTER_FUNC          CreateAdapter{nullptr};
    WINTUN_CLOSE_ADAPTER_FUNC           CloseAdapter{nullptr};
    WINTUN_START_SESSION_FUNC           StartSession{nullptr};
    WINTUN_END_SESSION_FUNC             EndSession{nullptr};
    WINTUN_GET_READ_WAIT_EVENT_FUNC     GetReadWaitEvent{nullptr};
    WINTUN_RECEIVE_PACKET_FUNC          ReceivePacket{nullptr};
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC  ReleaseReceivePacket{nullptr};
    WINTUN_ALLOCATE_SEND_PACKET_FUNC    AllocateSendPacket{nullptr};
    WINTUN_SEND_PACKET_FUNC             SendPacket{nullptr};

    bool load() {
        dll = LoadLibraryW(L"wintun.dll");
        if (!dll) return false;

        CreateAdapter       = (WINTUN_CREATE_ADAPTER_FUNC)      GetProcAddress(dll, "WintunCreateAdapter");
        CloseAdapter        = (WINTUN_CLOSE_ADAPTER_FUNC)       GetProcAddress(dll, "WintunCloseAdapter");
        StartSession        = (WINTUN_START_SESSION_FUNC)       GetProcAddress(dll, "WintunStartSession");
        EndSession          = (WINTUN_END_SESSION_FUNC)         GetProcAddress(dll, "WintunEndSession");
        GetReadWaitEvent    = (WINTUN_GET_READ_WAIT_EVENT_FUNC) GetProcAddress(dll, "WintunGetReadWaitEvent");
        ReceivePacket       = (WINTUN_RECEIVE_PACKET_FUNC)      GetProcAddress(dll, "WintunReceivePacket");
        ReleaseReceivePacket= (WINTUN_RELEASE_RECEIVE_PACKET_FUNC)GetProcAddress(dll, "WintunReleaseReceivePacket");
        AllocateSendPacket  = (WINTUN_ALLOCATE_SEND_PACKET_FUNC)GetProcAddress(dll, "WintunAllocateSendPacket");
        SendPacket          = (WINTUN_SEND_PACKET_FUNC)         GetProcAddress(dll, "WintunSendPacket");

        return CreateAdapter && CloseAdapter && StartSession && EndSession &&
               GetReadWaitEvent && ReceivePacket && ReleaseReceivePacket &&
               AllocateSendPacket && SendPacket;
    }

    void unload() {
        if (dll) {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }
};

#endif // _WIN32
