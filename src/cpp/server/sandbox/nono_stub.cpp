#include "lemon/sandbox/nono_ffi.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

struct nono_capability_set {
    std::vector<std::string> read_paths;
    std::vector<std::string> write_paths;
    std::vector<std::string> devices;
    bool allow_egress{false};
    bool allow_loopback{true};
    uint16_t bind_port{0};
};

namespace {
thread_local std::string g_last_error;
void set_last_error(const std::string& err) { g_last_error = err; }
void clear_last_error() { g_last_error.clear(); }
} // namespace

extern "C" {

nono_capability_set* nono_capability_set_new(void) {
    clear_last_error();
    return new (std::nothrow) nono_capability_set();
}

void nono_capability_set_free(nono_capability_set* caps) {
    delete caps;
}

nono_status nono_capability_add_fs_read(nono_capability_set* caps, const char* path) {
    if (!caps || !path || path[0] == '\0') {
        set_last_error("Invalid capability set or path parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->read_paths.emplace_back(path);
    return NONO_OK;
}

nono_status nono_capability_add_fs_write(nono_capability_set* caps, const char* path) {
    if (!caps || !path || path[0] == '\0') {
        set_last_error("Invalid capability set or path parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->write_paths.emplace_back(path);
    return NONO_OK;
}

nono_status nono_capability_add_device(nono_capability_set* caps, const char* dev_path) {
    if (!caps || !dev_path || dev_path[0] == '\0') {
        set_last_error("Invalid capability set or device path parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->devices.emplace_back(dev_path);
    return NONO_OK;
}

nono_status nono_capability_set_network_egress(nono_capability_set* caps, bool allow_egress) {
    if (!caps) {
        set_last_error("Invalid capability set parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->allow_egress = allow_egress;
    return NONO_OK;
}

nono_status nono_capability_set_network_loopback(nono_capability_set* caps, bool allow_loopback) {
    if (!caps) {
        set_last_error("Invalid capability set parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->allow_loopback = allow_loopback;
    return NONO_OK;
}

nono_status nono_capability_set_bind_port(nono_capability_set* caps, uint16_t port) {
    if (!caps) {
        set_last_error("Invalid capability set parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    caps->bind_port = port;
    return NONO_OK;
}

size_t nono_capability_get_read_path_count(const nono_capability_set* caps) {
    return caps ? caps->read_paths.size() : 0;
}

const char* nono_capability_get_read_path(const nono_capability_set* caps, size_t index) {
    if (!caps || index >= caps->read_paths.size()) return nullptr;
    return caps->read_paths[index].c_str();
}

size_t nono_capability_get_write_path_count(const nono_capability_set* caps) {
    return caps ? caps->write_paths.size() : 0;
}

const char* nono_capability_get_write_path(const nono_capability_set* caps, size_t index) {
    if (!caps || index >= caps->write_paths.size()) return nullptr;
    return caps->write_paths[index].c_str();
}

size_t nono_capability_get_device_count(const nono_capability_set* caps) {
    return caps ? caps->devices.size() : 0;
}

const char* nono_capability_get_device(const nono_capability_set* caps, size_t index) {
    if (!caps || index >= caps->devices.size()) return nullptr;
    return caps->devices[index].c_str();
}

bool nono_capability_get_network_egress(const nono_capability_set* caps) {
    return caps ? caps->allow_egress : false;
}

bool nono_capability_get_network_loopback(const nono_capability_set* caps) {
    return caps ? caps->allow_loopback : false;
}

uint16_t nono_capability_get_bind_port(const nono_capability_set* caps) {
    return caps ? caps->bind_port : 0;
}

nono_status nono_sandbox_apply(const nono_capability_set* caps) {
    if (!caps) {
        set_last_error("Invalid capability set parameter");
        return NONO_ERROR_INVALID_PARAM;
    }
    set_last_error("nono kernel sandboxing is not linked (running in fallback mode)");
    return NONO_ERROR_UNSUPPORTED;
}

bool nono_is_supported(void) {
    return false;
}

const char* nono_get_backend_name(void) {
    return "nono-stub";
}

const char* nono_status_to_string(nono_status status) {
    switch (status) {
        case NONO_OK:                     return "NONO_OK";
        case NONO_ERROR_GENERIC:          return "NONO_ERROR_GENERIC";
        case NONO_ERROR_UNSUPPORTED:      return "NONO_ERROR_UNSUPPORTED";
        case NONO_ERROR_INVALID_PARAM:    return "NONO_ERROR_INVALID_PARAM";
        case NONO_ERROR_APPLY_FAILED:     return "NONO_ERROR_APPLY_FAILED";
        case NONO_ERROR_PERMISSION_DENIED:return "NONO_ERROR_PERMISSION_DENIED";
        case NONO_ERROR_ALREADY_APPLIED:  return "NONO_ERROR_ALREADY_APPLIED";
    }
    return "NONO_ERROR_UNKNOWN";
}

const char* nono_get_last_error(void) {
    return g_last_error.c_str();
}

} // extern "C"
