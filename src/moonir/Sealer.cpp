#include "Sealer.h"

#include "ControlFlowBuilder.h"
#include "Verifier.h"

#include <utility>

namespace moon {

namespace {

bool isConcreteExecutable(const FunctionDecl& function) {
    return !function.isExtern && !function.isSelector &&
        (!function.isKernel || function.isCodegenReachable) &&
        (function.typeParams.empty() || function.isTemplateInstance);
}

} // namespace

bool Sealer::sealFunctionBodies(Module& module) {
    mErrors.clear();
    struct PendingBody {
        FunctionDecl* function = nullptr;
        std::unique_ptr<ControlFlowGraph> graph;
    };
    std::vector<PendingBody> pending;

    const auto prepare = [this, &module, &pending](FunctionDecl& function) {
        if (!isConcreteExecutable(function)) return;
        if (static_cast<bool>(function.body) ==
            static_cast<bool>(function.controlFlow)) {
            mErrors.push_back(
                "function '" + function.name +
                "' must own exactly one construction or canonical body");
            return;
        }
        if (function.controlFlow) return;

        ControlFlowBuilder builder;
        auto graph = builder.build(
            *function.body, function.params, RegionKind::Function, module);
        if (!graph) {
            for (const auto& error : builder.errors())
                mErrors.push_back(
                    "function '" + function.name + "': " + error);
            return;
        }

        Verifier verifier;
        if (!verifier.verify(*graph, module)) {
            for (const auto& diagnostic : verifier.errors())
                mErrors.push_back(
                    "function '" + function.name + "': " +
                    diagnostic.message);
            return;
        }
        pending.push_back({&function, std::move(graph)});
    };

    for (auto& declaration : module.declarations) {
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get())) {
            prepare(*function);
        } else if (auto* implementation =
                       dynamic_cast<ImplDecl*>(declaration.get())) {
            for (auto& method : implementation->methods)
                if (method) prepare(*method);
        }
    }

    if (!mErrors.empty()) return false;
    for (auto& item : pending) {
        item.function->controlFlow = std::move(item.graph);
        item.function->body.reset();
    }
    return true;
}

} // namespace moon
