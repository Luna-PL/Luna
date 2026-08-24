#include "driver/MoonGeneration.h"

#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "moonir/ContainerModel.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace luna::driver {
namespace {

struct LoadedMoonGeneration {
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
    std::shared_ptr<LunaJitModule> jitLease;
};

std::string containerDigest(const std::vector<uint8_t>& bytes) {
    llvm::SHA256 hash;
    hash.update(llvm::ArrayRef<uint8_t>(bytes));
    const auto digest = hash.final();
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

} // namespace

bool stageVerifiedMoonGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error) {
    auto loaded = std::make_shared<LoadedMoonGeneration>();
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            loaded->manifest, loaded->module, error))
        return false;
    loaded->contentDigest = containerDigest(containerBytes);

    CodeGenerator codeGenerator(
        loaded->manifest.packageId + "@" + loaded->contentDigest);
    if (!codeGenerator.generate(&loaded->module)) {
        std::ostringstream details;
        details << "verified Moon generation failed LLVM lowering";
        for (const auto& diagnostic : codeGenerator.errors())
            details << "\n" << diagnostic::render(diagnostic);
        error = details.str();
        return false;
    }
    loaded->jitLease = codeGenerator.materializeJitModule(error);
    if (!loaded->jitLease) {
        error = "verified Moon generation failed JIT materialization: " + error;
        return false;
    }

    luna::runtime::GenerationStagingRequest request;
    request.moduleId = loaded->manifest.packageId;
    request.contentDigest = loaded->contentDigest;
    request.moduleLease = loaded;
    return runtime.stage(
        request,
        [loaded](const auto& candidate, std::string& verificationError) {
            if (candidate.moduleId != loaded->manifest.packageId ||
                candidate.contentDigest != loaded->contentDigest) {
                verificationError =
                    "verified Moon generation identity changed before staging";
                return false;
            }
            return true;
        },
        [loaded](const auto&, auto& bindings, std::string& resolutionError) {
            std::vector<moon::DeclarationRef> published;
            published.reserve(loaded->module.exports.size() + 1);
            for (const auto& exported : loaded->module.exports)
                published.push_back(exported.declaration);
            if (loaded->manifest.entrypoint.complete() &&
                std::find(published.begin(), published.end(),
                          loaded->manifest.entrypoint) == published.end())
                published.push_back(loaded->manifest.entrypoint);

            bindings.reserve(published.size());
            for (const auto& reference : published) {
                const auto* declaration =
                    loaded->module.findDeclaration(reference);
                if (!declaration) {
                    resolutionError =
                        "verified Moon publication lost its declaration descriptor";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = declaration->symbolId.value;
                binding.contractId = declaration->contractId.value;
                binding.implementation = declaration;
                binding.declarationKind =
                    static_cast<uint32_t>(declaration->kind) + 1;
                binding.flags = 0;
                if (declaration->kind == moon::DeclarationKind::Function) {
                    const auto function = loaded->module.functionsBySymbol.find(
                        declaration->linkageName);
                    if (function == loaded->module.functionsBySymbol.end() ||
                        !function->second) {
                        resolutionError =
                            "verified Moon function lost its executable body";
                        return false;
                    }
                    const std::string symbol = function->second->linkName.empty()
                        ? function->second->generatedSymbolName
                        : function->second->linkName;
                    binding.implementation =
                        loaded->jitLease->lookup(symbol, resolutionError);
                    if (!binding.implementation) {
                        resolutionError = "verified Moon function '" + symbol +
                            "' failed JIT lookup: " + resolutionError;
                        return false;
                    }
                    binding.flags =
                        luna::runtime::GenerationBindingCallable;
                }
                bindings.push_back(std::move(binding));
            }
            return true;
        },
        initializer, staged, error);
}

} // namespace luna::driver
