#pragma once

#include <stdexcept>
#include <string>

// Pure musl backend asset resolution. A musl build of lemond must never resolve a
// glibc ("ubuntu"/"Ubuntu") release asset, and the GPU backends that only ship
// glibc binaries (llamacpp cuda/rocm, whispercpp rocm) have no musl build at all.
// These helpers encode exactly which musl assets exist, throwing for variants that
// don't, so the selection layer (descriptor support rows) and the install layer
// agree and the logic stays unit-testable off musl.
//
// Kept dependency-free (no SystemInfo, no I/O, no json) on purpose. Repos stay as
// lemonade-sdk/* here; apply_repo_owner_override() remaps them downstream.
namespace lemon::backends::musl {

enum class Arch { X86_64, Aarch64 };

struct Asset {
    std::string repo;      // GitHub "org/repo"
    std::string filename;  // Release asset filename
};

// Host architecture as an Arch. Backends pass this so the resolvers below stay
// pure and testable for either arch regardless of the build host.
inline Arch host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return Arch::Aarch64;
#else
    return Arch::X86_64;
#endif
}

inline Asset llamacpp(const std::string& backend, const std::string& version, Arch arch) {
    const std::string a = (arch == Arch::Aarch64) ? "arm64" : "x64";
    if (backend == "cpu") {
        return {"lemonade-sdk/llama.cpp", "llama-" + version + "-bin-linux-musl-" + a + ".tar.gz"};
    }
    if (backend == "vulkan") {
        return {"lemonade-sdk/llama.cpp", "llama-" + version + "-bin-linux-musl-vulkan-" + a + ".tar.gz"};
    }
    throw std::runtime_error("llamacpp backend '" + backend + "' has no musl build");
}

inline Asset whispercpp(const std::string& backend, const std::string& version, Arch arch) {
    const std::string a = (arch == Arch::Aarch64) ? "aarch64" : "x86_64";
    if (backend == "cpu") {
        return {"lemonade-sdk/whisper.cpp-rocm", "whisper-" + version + "-linux-musl-cpu-" + a + ".tar.gz"};
    }
    if (backend == "vulkan") {
        return {"lemonade-sdk/whisper.cpp-rocm", "whisper-" + version + "-linux-musl-vulkan-" + a + ".tar.gz"};
    }
    throw std::runtime_error("whispercpp backend '" + backend + "' has no musl build");
}

// sd.cpp ships only CPU and Vulkan on musl; load() coerces any GPU backend to one
// of those, and an explicit cuda/rocm request maps to the CPU asset here (never a
// glibc one). short_version is the asset-name form (see SDServer::get_install_params).
inline Asset sdcpp(const std::string& backend, const std::string& short_version, Arch arch) {
    const std::string a = (arch == Arch::Aarch64) ? "aarch64" : "x86_64";
    const std::string variant = (backend == "vulkan") ? "-vulkan-" : "-";
    return {"lemonade-sdk/stable-diffusion.cpp",
            "sd-" + short_version + "-bin-Linux-musl" + variant + a + ".zip"};
}

inline Asset kokoro(const std::string& backend, Arch arch) {
    const std::string a = (arch == Arch::Aarch64) ? "arm64" : "x86_64";
    if (backend == "cpu") {
        return {"lemonade-sdk/Kokoros", "kokoros-linux-musl-" + a + ".tar.gz"};
    }
    throw std::runtime_error("kokoro backend '" + backend + "' has no musl build");
}

inline Asset moonshine(const std::string& version, Arch arch) {
    const std::string a = (arch == Arch::Aarch64) ? "arm64" : "x64";
    return {"lemonade-sdk/moonshine-server-rocm",
            "moonshine-server-" + version + "-linux-musl-" + a + ".tar.gz"};
}

}  // namespace lemon::backends::musl
