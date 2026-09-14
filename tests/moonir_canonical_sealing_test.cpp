#include "moonir_canonical_test_support.h"

namespace canonical_test {

int runSealingTests(
    moon::ControlFlowBuilder& cfgBuilder,
    moon::Verifier& cfgVerifier,
    moon::Module& module,
    moon::Module& reverse,
    const moon::TypeRef& shortId,
    const moon::TypeRef& productId) {
    moon::Verifier verifier;
    SealingTestContext context{
        cfgBuilder, cfgVerifier, verifier, module, reverse,
        shortId, productId};
    if (const int result = runFunctionSealingTests(context)) return result;
    if (const int result = runCompositionSealingTests(context)) return result;
    if (const int result = runLoweredCompositionTests(context)) return result;
    if (const int result = runSymbolSealingTests(context)) return result;
    if (const int result = runIteratorSealingTests(context)) return result;
    return 0;
}

} // namespace canonical_test
