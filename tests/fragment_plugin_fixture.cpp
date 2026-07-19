#include "runtime/FragmentPluginABI.h"

namespace {

int traceEntry(const LunaFragmentInvocationV1* invocation) {
    if (!invocation || invocation->abi_version != LUNA_FRAGMENT_PLUGIN_ABI_V1 ||
        invocation->arg_count != 1 || !invocation->args || !invocation->args[0])
        return LUNA_FRAGMENT_PLUGIN_ERROR;
    const int value = *static_cast<const int*>(invocation->args[0]);
    return value == 41 ? LUNA_FRAGMENT_PLUGIN_CONTINUE : LUNA_FRAGMENT_PLUGIN_ERROR;
}

const LunaFragmentPluginDescriptorV1 descriptor = {
    LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1,
    LUNA_FRAGMENT_PLUGIN_ABI_V1,
    sizeof(LunaFragmentPluginDescriptorV1),
    "fixture.plugin",
    "external_trace",
    "pipeline",
    "luna.slot.pipeline.interceptor.once.i32.v1",
    LUNA_FRAGMENT_KIND_INTERCEPTOR,
    LUNA_FRAGMENT_CARDINALITY_ONCE,
    LUNA_FRAGMENT_EFFECT_HOST_ONLY,
    traceEntry,
};

} // namespace

extern "C" const LunaFragmentPluginDescriptorV1*
luna_fragment_plugin_descriptor_v1() {
    return &descriptor;
}
