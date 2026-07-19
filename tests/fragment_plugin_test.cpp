#include "runtime/Runtime.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: fragment-plugin-test <plugin>\n";
        return 2;
    }
    if (!rt_fragment_plugin_load(argv[1])) {
        std::cerr << "load failed: " << rt_fragment_plugin_last_error() << "\n";
        return 1;
    }

    constexpr const char* slot = "pipeline";
    constexpr const char* fragment = "external_trace";
    constexpr const char* contract = "luna.slot.pipeline.interceptor.once.i32.v1";
    if (!rt_fragment_plugin_is_registered(slot, fragment, contract)) {
        std::cerr << "valid external fragment was not registered\n";
        return 1;
    }
    if (rt_fragment_plugin_is_registered(slot, fragment, "wrong-contract")) {
        std::cerr << "contract mismatch was accepted\n";
        return 1;
    }

    int value = 41;
    const void* args[] = {&value};
    LunaFragmentInvocationV1 invocation{
        LUNA_FRAGMENT_PLUGIN_ABI_V1, args, 1};
    if (rt_fragment_plugin_invoke(slot, fragment, contract, &invocation) !=
        LUNA_FRAGMENT_PLUGIN_CONTINUE) {
        std::cerr << "external fragment invocation failed: "
                  << rt_fragment_plugin_last_error() << "\n";
        return 1;
    }

    if (rt_fragment_plugin_invoke(slot, fragment, "wrong-contract", &invocation) !=
        LUNA_FRAGMENT_PLUGIN_ERROR) {
        std::cerr << "invocation bypassed contract validation\n";
        return 1;
    }
    std::cout << "external fragment plugin ABI validated\n";
    return 0;
}
