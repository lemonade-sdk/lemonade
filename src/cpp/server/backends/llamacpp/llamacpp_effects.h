#pragma once

#ifdef LEMONADE_LLAMACPP_ORACLE

#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/wrapped_server.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace lemon::backends::llamacpp::oracle {

class Effects {
public:
    virtual ~Effects() = default;
    virtual RuntimeConfig* runtime_config() = 0;
    virtual json versions() = 0;
    virtual std::string rocm_arch() = 0;
    virtual std::string cuda_arch() = 0;
    virtual std::string unsupported(const std::string& backend) = 0;
    virtual std::string os() = 0;
    virtual SystemInfo::SupportedBackendsResult supported_backends(const std::string& recipe) = 0;
    virtual void install(const std::string& recipe, const std::string& backend) = 0;
    virtual int port() = 0;
    virtual std::string path_executable(const std::string& binary) = 0;
    virtual std::string external_binary(const std::string& recipe, const std::string& backend) = 0;
    virtual std::string install_directory(const std::string& recipe, const std::string& backend) = 0;
    virtual std::string installed_binary(const std::string& dir, const std::string& binary) = 0;
    virtual std::vector<std::string> runtime_paths(const std::string& arch) = 0;
    virtual void stage(const std::string& arch, const std::filesystem::path& dir) = 0;
    virtual const char* environment(const std::string& name) = 0;
    virtual std::string cuda_visible_devices(const std::string& arch) = 0;
    virtual std::string home() = 0;
    virtual void start(const ServerCommand& command, ProcessOutput output,
                       const std::string& cwd) = 0;
    virtual json post(const std::string& endpoint, const json& request) = 0;
    virtual json get(const std::string& endpoint) = 0;
    virtual void stream(const std::string& endpoint, const std::string& body,
                        httplib::DataSink& sink, bool sse, long timeout,
                        WrappedServer::TelemetryCallback callback) = 0;
    virtual int version_command(const std::string& command, std::string& output) = 0;
    virtual FILE* version_pipe(const std::string& command) = 0;
    virtual int close_version_pipe(FILE* pipe) = 0;
    virtual bool exists(const std::filesystem::path& path) = 0;
    virtual bool regular(const std::filesystem::path& path) = 0;
    virtual bool executable(const std::filesystem::path& path) = 0;
    virtual void observe(const std::string& hook) = 0;

    std::string environment_string(const std::string& name) {
        const char* value = environment(name);
        return value ? value : "";
    }
};

inline thread_local Effects* current_effects = nullptr;

inline Effects& effects() {
    if (!current_effects) throw std::logic_error("llama.cpp oracle has no effect provider");
    return *current_effects;
}


}

// Compile the real decision branches on CPU without defining native system headers.
#define LLAMA_WINDOWS (LEMONADE_LLAMACPP_ORACLE_OS == 2)
#define LLAMA_LINUX (LEMONADE_LLAMACPP_ORACLE_OS == 1)
#define LLAMA_MACOS (LEMONADE_LLAMACPP_ORACLE_OS == 3)
#define LLAMA_ARM64 LEMONADE_LLAMACPP_ORACLE_ARM64
#define LLAMA_EFFECT(original, ...) __VA_ARGS__
#define LLAMA_OBSERVE(hook) lemon::backends::llamacpp::oracle::effects().observe(hook)
#define LLAMA_POST(...) lemon::backends::llamacpp::oracle::effects().post(__VA_ARGS__)
#define LLAMA_GET(...) lemon::backends::llamacpp::oracle::effects().get(__VA_ARGS__)

#else

#ifdef _WIN32
#define LLAMA_WINDOWS 1
#else
#define LLAMA_WINDOWS 0
#endif
#ifdef __linux__
#define LLAMA_LINUX 1
#else
#define LLAMA_LINUX 0
#endif
#ifdef __APPLE__
#define LLAMA_MACOS 1
#else
#define LLAMA_MACOS 0
#endif
#ifdef __aarch64__
#define LLAMA_ARM64 1
#else
#define LLAMA_ARM64 0
#endif
#define LLAMA_EFFECT(original, ...) original
#define LLAMA_OBSERVE(hook)
#define LLAMA_POST(...) forward_request(__VA_ARGS__)
#define LLAMA_GET(...) forward_get_request(__VA_ARGS__)

#endif
