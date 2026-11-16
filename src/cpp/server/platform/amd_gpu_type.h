#pragma once

#ifdef __linux__
#include <cstdint>
#include <filesystem>
#include <string>
#include <fcntl.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#include <unistd.h>
#include <system_error>

namespace lemon::gpu_metrics_detail {

// libdrm's fusion flag distinguishes APUs reliably where board_info does not.
inline bool is_amd_apu(const std::filesystem::path& card_path,
                       std::string* marketing_name = nullptr,
                       bool use_board_info_fallback = false,
                       bool* confirmed = nullptr) {
    if (confirmed) *confirmed = false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path drm_dir = card_path / "device" / "drm";
    if (fs::is_directory(drm_dir, ec)) {
        for (fs::directory_iterator it(drm_dir, ec), end;
             it != end && !ec; it.increment(ec)) {
            const std::string render_name = it->path().filename().string();
            if (render_name.rfind("renderD", 0) != 0) continue;
            const fs::path render_node = fs::path("/dev/dri") / render_name;
            const int fd = open(render_node.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            uint32_t major = 0, minor = 0;
            amdgpu_device_handle handle = nullptr;
            const int initialized = amdgpu_device_initialize(fd, &major, &minor, &handle);
            if (initialized != 0 || !handle) {
                close(fd);
                continue;
            }
            amdgpu_gpu_info info{};
            const int queried = amdgpu_query_gpu_info(handle, &info);
            if (queried == 0 && marketing_name) {
                const char* name = amdgpu_get_marketing_name(handle);
                if (name) *marketing_name = name;
            }
            amdgpu_device_deinitialize(handle);
            close(fd);
            if (queried == 0) {
                if (confirmed) *confirmed = true;
                return (info.ids_flags & AMDGPU_IDS_FLAGS_FUSION) != 0;
            }
        }
    }
    // SystemInfo's eviction monitor retains its original conservative fallback.
    // The board_info guess is used only by the display metrics in sandboxes.
    return use_board_info_fallback &&
        !fs::exists(card_path / "device" / "board_info", ec);
}

} // namespace lemon::gpu_metrics_detail
#endif
