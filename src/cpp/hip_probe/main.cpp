// Prints HIP's device order on Windows, one "index<TAB>pci_bus_id<TAB>name" line
// per device. lemond runs this as a short-lived child: HIP numbers GPUs in a
// driver order that WMI does not expose, and initializing HIP inside the server
// would keep a HIP context (and a dGPU awake) for the server's lifetime and let a
// driver fault take the server down.

#include <windows.h>

#include <string>

int main() {
    // The display driver installs the HIP runtime in System32. Loading it only from
    // there keeps a DLL beside the executable from standing in for it.
    HMODULE hip = nullptr;
    for (const wchar_t* dll : {L"amdhip64_7.dll", L"amdhip64_6.dll"}) {
        hip = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (hip) break;
    }
    if (!hip) return 2;

    using GetDeviceCount = int (*)(int*);
    using GetDeviceString = int (*)(char*, int, int);
    auto get_count = reinterpret_cast<GetDeviceCount>(GetProcAddress(hip, "hipGetDeviceCount"));
    auto get_name = reinterpret_cast<GetDeviceString>(GetProcAddress(hip, "hipDeviceGetName"));
    auto get_bus = reinterpret_cast<GetDeviceString>(GetProcAddress(hip, "hipDeviceGetPCIBusId"));
    int count = 0;
    if (!get_count || !get_name || !get_bus || get_count(&count) != 0) return 3;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (int i = 0; i < count; ++i) {
        char name[256] = {0};
        char bus[64] = {0};
        get_name(name, sizeof(name), i);
        get_bus(bus, sizeof(bus), i);
        const std::string line = std::to_string(i) + "\t" + bus + "\t" + name + "\n";
        DWORD written = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    return 0;
}
