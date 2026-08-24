#include "driver/NativeGeneration.h"

#include "driver/NativeArtifact.h"

#include <memory>
#include <utility>
#include <vector>

namespace luna::driver {

static_assert(
    static_cast<uint32_t>(luna::runtime::GenerationBindingCallable) ==
        static_cast<uint32_t>(LUNA_NATIVE_EXPORT_CALLABLE_V1),
    "Native and internal generation callable flags must agree");

bool stageVerifiedNativeGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error) {
    auto library = std::make_shared<VerifiedNativeLibrary>();
    if (!loadVerifiedNativeLibrary(
            artifactPath, trustStorePath, *library, error))
        return false;

    luna::runtime::GenerationStagingRequest request;
    request.moduleId = library->proof().packageId;
    request.contentDigest = nativeDigestHex(library->proof().artifactDigest);
    request.moduleLease = library;
    return runtime.stage(
        request,
        [library](const auto& candidate, std::string& verificationError) {
            if (!*library ||
                candidate.moduleId != library->proof().packageId ||
                candidate.contentDigest != nativeDigestHex(
                    library->proof().artifactDigest)) {
                verificationError =
                    "verified Native generation identity changed before staging";
                return false;
            }
            return true;
        },
        [library](const auto&, auto& bindings, std::string& resolutionError) {
            bindings.reserve(static_cast<size_t>(library->exportCount()));
            for (uint64_t index = 0; index < library->exportCount(); ++index) {
                const auto* exported = library->exportAt(index);
                if (!exported) {
                    resolutionError =
                        "verified Native registry changed during resolution";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = exported->symbol_id;
                binding.contractId = exported->contract_id;
                binding.implementation = exported->entry
                    ? exported->entry : static_cast<const void*>(exported);
                binding.declarationKind = exported->declaration_kind;
                binding.flags = exported->flags;
                bindings.push_back(std::move(binding));
            }
            return true;
        },
        initializer, staged, error);
}

} // namespace luna::driver
