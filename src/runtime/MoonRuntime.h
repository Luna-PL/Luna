#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace luna::runtime {

enum GenerationBindingFlag : uint32_t {
    GenerationBindingCallable = 1u << 0,
};

// Internal 0.3 generation state-machine input. Public source/API spelling is
// deliberately deferred by TBD-EV004; this header is not an installed ABI.
struct GenerationStagingRequest {
    std::string moduleId;
    std::string contentDigest;
    std::shared_ptr<const void> moduleLease;
};

struct GenerationBinding {
    std::string symbolId;
    std::string contractId;
    const void* implementation = nullptr;
    uint32_t declarationKind = 0;
    uint32_t flags = 0;
};

using GenerationVerifier = std::function<bool(
    const GenerationStagingRequest&, std::string&)>;
using GenerationResolver = std::function<bool(
    const GenerationStagingRequest&, std::vector<GenerationBinding>&,
    std::string&)>;
using GenerationInitializer = std::function<bool(
    const GenerationStagingRequest&, const std::vector<GenerationBinding>&,
    std::string&)>;

class MoonRuntime {
    struct GenerationState;
    struct ModuleState;

public:
    class SafePoint {
    public:
        SafePoint(const SafePoint&) = delete;
        SafePoint& operator=(const SafePoint&) = delete;
        SafePoint(SafePoint&& other) noexcept;
        SafePoint& operator=(SafePoint&& other) noexcept;

    private:
        friend class MoonRuntime;
        explicit SafePoint(const MoonRuntime* owner) : owner_(owner) {}
        const MoonRuntime* owner_ = nullptr;
        bool consumed_ = false;
    };

    class StagedGeneration {
    public:
        StagedGeneration() = default;
        StagedGeneration(const StagedGeneration&) = delete;
        StagedGeneration& operator=(const StagedGeneration&) = delete;
        StagedGeneration(StagedGeneration&&) noexcept = default;
        StagedGeneration& operator=(StagedGeneration&&) noexcept = default;

        explicit operator bool() const { return generation_ != nullptr; }
        uint64_t generationId() const;
        const std::string& moduleId() const;
        const std::string& contentDigest() const;

    private:
        friend class MoonRuntime;
        const MoonRuntime* owner_ = nullptr;
        std::shared_ptr<const GenerationState> generation_;
    };

    class PinnedBinding {
    public:
        explicit operator bool() const { return binding_ != nullptr; }
        uint64_t generationId() const;
        const std::string& symbolId() const;
        const std::string& contractId() const;
        const void* implementation() const;
        uint32_t declarationKind() const;
        uint32_t flags() const;

    private:
        friend class MoonRuntime;
        friend class PinnedGeneration;
        friend class SwitchableBinding;
        std::shared_ptr<const GenerationState> generation_;
        const GenerationBinding* binding_ = nullptr;
    };

    class PinnedGeneration {
    public:
        explicit operator bool() const { return generation_ != nullptr; }
        uint64_t generationId() const;
        const std::string& moduleId() const;
        const std::string& contentDigest() const;
        PinnedBinding find(const std::string& symbolId,
                           const std::string& contractId) const;

    private:
        friend class MoonRuntime;
        std::shared_ptr<const GenerationState> generation_;
    };

    class SwitchableBinding {
    public:
        explicit operator bool() const { return module_ != nullptr; }
        PinnedBinding pin() const;
        const std::string& symbolId() const { return symbolId_; }
        const std::string& contractId() const { return contractId_; }

    private:
        friend class MoonRuntime;
        std::shared_ptr<ModuleState> module_;
        std::string symbolId_;
        std::string contractId_;
    };

    SafePoint safePoint() const { return SafePoint(this); }

    bool stage(const GenerationStagingRequest& request,
               const GenerationVerifier& verifier,
               const GenerationResolver& resolver,
               const GenerationInitializer& initializer,
               StagedGeneration& staged, std::string& error);
    bool activate(StagedGeneration& staged, SafePoint& safePoint,
                  std::string& error);
    bool rollback(const std::string& moduleId, uint64_t generationId,
                  SafePoint& safePoint, std::string& error);

    PinnedGeneration pin(const std::string& moduleId) const;
    bool makeSwitchable(const std::string& moduleId,
                        const std::string& symbolId,
                        const std::string& contractId,
                        SwitchableBinding& binding, std::string& error);
    uint64_t activeGenerationId(const std::string& moduleId) const;
    size_t retainedGenerationCount(const std::string& moduleId) const;

private:
    static const GenerationBinding* findBinding(
        const GenerationState& generation, const std::string& symbolId,
        const std::string& contractId);
    static PinnedBinding makePinnedBinding(
        std::shared_ptr<const GenerationState> generation,
        const std::string& symbolId, const std::string& contractId);
    static bool validatesRequirements(
        const ModuleState& module, const GenerationState& generation,
        std::string& error);
    bool consumeSafePoint(SafePoint& safePoint, std::string& error) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ModuleState>> modules_;
    uint64_t nextGenerationId_ = 1;
};

} // namespace luna::runtime
