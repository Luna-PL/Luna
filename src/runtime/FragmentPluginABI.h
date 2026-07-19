#pragma once

#include <stddef.h>
#include <stdint.h>

// Stable C ABI for the first external fragment-plugin boundary.  The ABI is
// deliberately metadata-first: the host validates the slot contract before
// it ever calls an entry point.  v1 supports host-only, single-shot
// interceptors with explicit arguments.  It does not expose a Luna stack
// frame, so a plugin cannot retain a borrowed continuation accidentally.
#ifdef __cplusplus
extern "C" {
#endif

#define LUNA_FRAGMENT_PLUGIN_ABI_V1 1u
#define LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1 0x4c465031u /* "LFP1" */

enum {
    LUNA_FRAGMENT_KIND_INTERCEPTOR = 1u,
    LUNA_FRAGMENT_KIND_CONTEXT = 2u,
};

enum {
    LUNA_FRAGMENT_CARDINALITY_ONCE = 1u,
    LUNA_FRAGMENT_CARDINALITY_MANY = 2u,
};

enum {
    LUNA_FRAGMENT_EFFECT_HOST_ONLY = 1u << 0,
    LUNA_FRAGMENT_EFFECT_MAY_ABORT = 1u << 1,
};

enum {
    LUNA_FRAGMENT_PLUGIN_CONTINUE = 0,
    LUNA_FRAGMENT_PLUGIN_ABORT = 1,
    LUNA_FRAGMENT_PLUGIN_ERROR = -1,
};

typedef struct LunaFragmentInvocationV1 {
    uint32_t abi_version;
    const void* const* args;
    size_t arg_count;
} LunaFragmentInvocationV1;

typedef int (*LunaFragmentPluginEntryV1)(
    const LunaFragmentInvocationV1* invocation);

typedef struct LunaFragmentPluginDescriptorV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t descriptor_size;
    const char* plugin_id;
    const char* fragment_name;
    const char* slot_name;
    // Canonical compiler-produced contract identity.  It covers the slot
    // kind, cardinality, and explicit parameter layout.
    const char* contract_hash;
    uint32_t fragment_kind;
    uint32_t cardinality;
    uint32_t effects;
    LunaFragmentPluginEntryV1 entry;
} LunaFragmentPluginDescriptorV1;

typedef const LunaFragmentPluginDescriptorV1* (*LunaFragmentPluginDescriptorFnV1)(void);

#ifdef __cplusplus
}
#endif
