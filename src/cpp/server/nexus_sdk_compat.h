#pragma once

// Route policy entry points of the Nexus client SDK that the vendored header
// (src/LemonadeNexusSDK, see UPSTREAM.json) does not declare yet. lemond resolves
// them from the loaded SDK library at run time, so it starts against an SDK with
// or without them. Nothing here redeclares an ln_* symbol: the names below are
// lemond's own, so the file compiles against the vendored header and against a
// newer one that declares the functions. Once the vendored SDK exports them,
// replace route_policy_api() with direct calls and delete this file.
//
// The SDK's C ABI for these functions is:
//   ln_error_t ln_mesh_service_set_catalog(ln_client_t*, uint16_t vport, const char* catalog_json);
//   ln_error_t ln_mesh_service_catalog(ln_client_t*, uint16_t vport, char** out_json);
//   typedef void (*ln_ingress_denied_fn)(char* event_json, void* user);  /* callback owns event_json */
//   ln_error_t ln_set_ingress_denied_callback(ln_client_t*, ln_ingress_denied_fn fn, void* user);

#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>

namespace lemon::nexus_sdk {

using set_catalog_fn = ln_error_t (*)(ln_client_t* client, uint16_t vport, const char* catalog_json);
using catalog_fn = ln_error_t (*)(ln_client_t* client, uint16_t vport, char** out_json);
using ingress_denied_fn = void (*)(char* event_json, void* user);
using set_ingress_denied_callback_fn = ln_error_t (*)(ln_client_t* client, ingress_denied_fn fn, void* user);

struct RoutePolicyApi {
    set_catalog_fn set_catalog{nullptr};
    catalog_fn catalog{nullptr};
    set_ingress_denied_callback_fn set_ingress_denied_callback{nullptr};
    // True when the loaded SDK exports all three functions. Each one is used only
    // when the whole set exists, so lemond never registers a catalog it cannot see
    // refusals for.
    bool available() const { return set_catalog && catalog && set_ingress_denied_callback; }
};

// Resolved once from the SDK library lemond is linked against. Thread-safe.
const RoutePolicyApi& route_policy_api();

} // namespace lemon::nexus_sdk
#endif
