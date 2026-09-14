#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "tooling/AnalysisSnapshot.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorSealingTests(SealingTestContext& context) {
    auto& verifier = context.verifier;

    const std::string affineCaptureSealerSource = R"luna(
fn use_string(s: string) -> i32 {
    return 0;
}
fn main() -> i32 {
    let captured = "luna";
    let read = fn() -> i32 {
        return use_string(captured);
    };
    return read();
}
)luna";
    auto affineCaptureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        affineCaptureSealerSource, "<affine-capture-sealer>");
    if (!affineCaptureSnapshot.success())
        return fail("frontend rejected the affine capture sealer source");
    moon::LunaLowerer affineCaptureLowerer;
    auto affineCaptureModule = affineCaptureLowerer.lower(
        *affineCaptureSnapshot.program(),
        *affineCaptureSnapshot.symbolTable());
    if (!affineCaptureLowerer.errors().empty()) {
        for (const auto& diagnostic : affineCaptureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the affine capture source");
    }
    moon::Sealer affineCaptureSealer;
    if (!affineCaptureSealer.sealFunctionBodies(*affineCaptureModule) ||
        !verifier.verify(*affineCaptureModule)) {
        for (const auto& error : affineCaptureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("affine capture function did not survive Sealer canonicalization");
    }
    // Verify the sealed lambda CFG does not retain an implicit FreeStmt for
    // the captured local as an operation block (it must be suppressed).
    bool sealedLambdaHasFreeStmt = false;
    for (const auto& declaration : affineCaptureModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || !function->controlFlow) continue;
        for (const auto& block : function->controlFlow->blocks) {
            for (const auto& operation : block.operations) {
                if (dynamic_cast<const moon::FreeStmt*>(operation.get()))
                    sealedLambdaHasFreeStmt = true;
            }
        }
    }
    if (sealedLambdaHasFreeStmt)
        return fail("sealed canonical CFG retained an implicit FreeStmt operation");

    // Materialized iterator recipe: a simple for-each over a Copy array must
    // survive Sealer canonicalization. The for-loop's into_iter() creates a
    // materialized recipe that the canonical CFG must expand into ordinary
    // locals, indexed access, and a backedge graph.
    const std::string iteratorSealerSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    for v in values {
        total = total + v;
    }
    return total;
}
)luna";
    auto iteratorSealerSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        iteratorSealerSource, "<iterator-sealer>");
    if (!iteratorSealerSnapshot.success())
        return fail("frontend rejected the iterator sealer source");
    moon::LunaLowerer iteratorSealerLowerer;
    auto iteratorSealerModule = iteratorSealerLowerer.lower(
        *iteratorSealerSnapshot.program(),
        *iteratorSealerSnapshot.symbolTable());
    if (!iteratorSealerLowerer.errors().empty()) {
        for (const auto& diagnostic : iteratorSealerLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the iterator sealer source");
    }
    moon::Sealer iteratorSealer;
    if (!iteratorSealer.sealFunctionBodies(*iteratorSealerModule) ||
        !verifier.verify(*iteratorSealerModule)) {
        for (const auto& error : iteratorSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Copy array for-each did not survive Sealer canonicalization");
    }

    // Capturing-closure iterator adapter: a map adapter that captures a Copy
    // local must survive Sealer canonicalization. The canonical CFG must
    // expand the map as an ordinary closure call with an environment pointer
    // rather than rejecting it as "capture-free callable only".
    const std::string capturingIterSource = R"luna(
fn main() -> i32 {
    let offset = 1;
    let values = [1, 2, 3, 4];
    let total = 0;
    for v in values.iter().map(fn(x: &i32) -> i32 { return *x + offset; }) {
        total = total + v;
    }
    return total;
}
)luna";
    auto capturingIterSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        capturingIterSource, "<capturing-iter>");
    if (!capturingIterSnapshot.success())
        return fail("frontend rejected the capturing iterator source");
    moon::LunaLowerer capturingIterLowerer;
    auto capturingIterModule = capturingIterLowerer.lower(
        *capturingIterSnapshot.program(),
        *capturingIterSnapshot.symbolTable());
    if (!capturingIterLowerer.errors().empty()) {
        for (const auto& diagnostic : capturingIterLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the capturing iterator source");
    }
    moon::Sealer capturingIterSealer;
    if (!capturingIterSealer.sealFunctionBodies(*capturingIterModule) ||
        !verifier.verify(*capturingIterModule)) {
        for (const auto& error : capturingIterSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("capturing-closure iterator did not survive Sealer canonicalization");
    }

    // Iterator terminals with closure callbacks: for_each and fold must
    // accept Closure-typed callables (not just Function) in the canonical
    // CFG. A lambda callback's type is Closure, not Function.
    const std::string terminalClosureSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    values.iter().for_each(fn(x: &i32) -> unit {
        total = total + *x;
    });
    if total > 0 { return total; }
    return 0;
}
)luna";
    auto terminalClosureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        terminalClosureSource, "<terminal-closure>");
    if (!terminalClosureSnapshot.success())
        return fail("frontend rejected the terminal closure source");
    moon::LunaLowerer terminalClosureLowerer;
    auto terminalClosureModule = terminalClosureLowerer.lower(
        *terminalClosureSnapshot.program(),
        *terminalClosureSnapshot.symbolTable());
    if (!terminalClosureLowerer.errors().empty()) {
        for (const auto& diagnostic : terminalClosureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the terminal closure source");
    }
    moon::Sealer terminalClosureSealer;
    if (!terminalClosureSealer.sealFunctionBodies(*terminalClosureModule) ||
        !verifier.verify(*terminalClosureModule)) {
        for (const auto& error : terminalClosureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("for_each with closure callback did not survive Sealer canonicalization");
    }

    // fold with a closure reducer
    const std::string foldClosureSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let result = values.into_iter().fold(0, fn(acc: i32, x: i32) -> i32 {
        return acc + x;
    });
    if result > 0 { return result; }
    return 0;
}
)luna";
    auto foldClosureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        foldClosureSource, "<fold-closure>");
    if (!foldClosureSnapshot.success())
        return fail("frontend rejected the fold closure source");
    moon::LunaLowerer foldClosureLowerer;
    auto foldClosureModule = foldClosureLowerer.lower(
        *foldClosureSnapshot.program(),
        *foldClosureSnapshot.symbolTable());
    if (!foldClosureLowerer.errors().empty()) {
        for (const auto& diagnostic : foldClosureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the fold closure source");
    }
    moon::Sealer foldClosureSealer;
    if (!foldClosureSealer.sealFunctionBodies(*foldClosureModule) ||
        !verifier.verify(*foldClosureModule)) {
        for (const auto& error : foldClosureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("fold with closure reducer did not survive Sealer canonicalization");
    }

    // Intrinsic call type resolution: a `slice` intrinsic call produces a
    // Slice-typed result. The MoonIR lowering must set the CallExpr type from
    // the Sema-inferred type so the canonical verifier's let-initializer type
    // check passes.
    const std::string sliceSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30, 40];
    let middle = slice(borrow values, 1, 3);
    return middle[1];
}
)luna";
    auto sliceSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        sliceSource, "<slice-sealer>");
    if (!sliceSnapshot.success())
        return fail("frontend rejected the slice sealer source");
    moon::LunaLowerer sliceLowerer;
    auto sliceModule = sliceLowerer.lower(
        *sliceSnapshot.program(), *sliceSnapshot.symbolTable());
    if (!sliceLowerer.errors().empty()) {
        for (const auto& diagnostic : sliceLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the slice sealer source");
    }
    moon::Sealer sliceSealer;
    if (!sliceSealer.sealFunctionBodies(*sliceModule) ||
        !verifier.verify(*sliceModule)) {
        for (const auto& error : sliceSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("slice intrinsic program did not survive Sealer canonicalization");
    }


    return 0;
}

} // namespace canonical_test
