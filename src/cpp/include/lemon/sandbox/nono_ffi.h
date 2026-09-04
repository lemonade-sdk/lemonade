#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nono_capability_set nono_capability_set;

typedef enum {
    NONO_OK = 0,
    NONO_ERROR_GENERIC = 1,
    NONO_ERROR_UNSUPPORTED = 2,
    NONO_ERROR_INVALID_PARAM = 3,
    NONO_ERROR_APPLY_FAILED = 4,
    NONO_ERROR_PERMISSION_DENIED = 5,
    NONO_ERROR_ALREADY_APPLIED = 6
} nono_status;

nono_capability_set* nono_capability_set_new(void);
void nono_capability_set_free(nono_capability_set* caps);

nono_status nono_capability_add_fs_read(nono_capability_set* caps, const char* path);
nono_status nono_capability_add_fs_write(nono_capability_set* caps, const char* path);
nono_status nono_capability_add_device(nono_capability_set* caps, const char* dev_path);
nono_status nono_capability_set_network_egress(nono_capability_set* caps, bool allow_egress);
nono_status nono_capability_set_network_loopback(nono_capability_set* caps, bool allow_loopback);
nono_status nono_capability_set_bind_port(nono_capability_set* caps, uint16_t port);

size_t nono_capability_get_read_path_count(const nono_capability_set* caps);
const char* nono_capability_get_read_path(const nono_capability_set* caps, size_t index);
size_t nono_capability_get_write_path_count(const nono_capability_set* caps);
const char* nono_capability_get_write_path(const nono_capability_set* caps, size_t index);
size_t nono_capability_get_device_count(const nono_capability_set* caps);
const char* nono_capability_get_device(const nono_capability_set* caps, size_t index);
bool nono_capability_get_network_egress(const nono_capability_set* caps);
bool nono_capability_get_network_loopback(const nono_capability_set* caps);
uint16_t nono_capability_get_bind_port(const nono_capability_set* caps);

nono_status nono_sandbox_apply(const nono_capability_set* caps);
bool nono_is_supported(void);
const char* nono_get_backend_name(void);
const char* nono_status_to_string(nono_status status);
const char* nono_get_last_error(void);

#ifdef __cplusplus
}
#endif
