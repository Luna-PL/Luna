#pragma once

#include "CoreContracts.h"
#include "Ownership.h"
#include "StableIdentity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luna::sysmeta {

// Sysmeta is compiler authority: source code may inspect stable projections
// in the future, but it can never construct, attach, or override these facts.
// Keep it typed so safety decisions never depend on user-controlled strings.
inline constexpr uint16_t SchemaMajor = 1;
inline constexpr uint16_t SchemaMinor = 3;
inline constexpr const char* DropTraitId =
    luna::core_contracts::canonical_0_3::DropTraitId;
inline constexpr const char* DropMethodName =
    luna::core_contracts::DropMethodName;
inline constexpr const char* FromTraitId =
    luna::core_contracts::canonical_0_3::FromTraitId;
inline constexpr const char* FromMethodName =
    luna::core_contracts::FromMethodName;
inline constexpr const char* ResultTypeId =
    luna::core_contracts::canonical_0_3::ResultTypeId;
inline constexpr const char* OptionTypeId =
    luna::core_contracts::canonical_0_3::OptionTypeId;
inline constexpr const char* IteratorTraitId =
    luna::core_contracts::canonical_0_3::IteratorTraitId;
inline constexpr const char* IntoIteratorTraitId =
    luna::core_contracts::canonical_0_3::IntoIteratorTraitId;
inline constexpr const char* FromIteratorTraitId =
    luna::core_contracts::canonical_0_3::FromIteratorTraitId;
inline constexpr const char* FromIteratorBeginMethodName =
    luna::core_contracts::FromIteratorBeginMethodName;
inline constexpr const char* FromIteratorPushMethodName =
    luna::core_contracts::FromIteratorPushMethodName;
inline constexpr const char* FromIteratorFinishMethodName =
    luna::core_contracts::FromIteratorFinishMethodName;

enum class ControlForm : uint8_t {
    Plain,
    Interceptor,
    Context,
    Coroutine,
};

enum class Cardinality : uint8_t {
    None,
    Once,
    Many,
};

enum class ContinuationStorage : uint8_t {
    None,
    ScopedStack,
    PersistentFrame,
};

enum class Forwarding : uint8_t {
    None,
    Automatic,
    Explicit,
};

enum class ResourceManagement : uint8_t {
    Value,
    Unique,
};

enum class ReleaseDomain : uint8_t {
    None,
    LunaGlobal,
    Foreign,
    Device,
    Executable,
    HostService,
};

// Declaration kind participates directly in ContractId. Keep one core enum
// for frontend queries, MoonIR, and container serialization so ordinals cannot
// drift between layers.
enum class DeclarationKind : uint8_t {
    Function,
    Fragment,
    Struct,
    Enum,
    Trait,
    Implementation,
    MetadataSchema,
    Slot,
};

enum class ResourceLifetime : uint8_t {
    Value,
    Lexical,
    Borrowed,
    Explicit,
};

struct IdentityFacts {
    luna::identity::TypeId type;
    luna::identity::ShapeId shape;
    luna::identity::SymbolId symbol;
    luna::identity::ContractId contract;
    luna::identity::AbiLayoutId abiLayout;
};

struct ControlFacts {
    ControlForm form = ControlForm::Plain;
    Cardinality cardinality = Cardinality::None;
    ContinuationStorage storage = ContinuationStorage::None;
    Forwarding forwarding = Forwarding::None;
    bool abortPermitted = false;
    bool replayValidated = false;
};

struct ResourceFacts {
    std::vector<luna::ownership::Contract> parameters;
    luna::ownership::Contract result;
    ResourceManagement management = ResourceManagement::Value;
    ReleaseDomain releaseDomain = ReleaseDomain::None;
    ResourceLifetime lifetime = ResourceLifetime::Value;
    luna::ownership::Relation relation =
        luna::ownership::Relation::Owned;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::CleanupAction cleanup =
        luna::ownership::CleanupAction::None;
    bool cleanupRequired = false;
    bool recursiveCleanup = false;
    bool needsDrop = false;
    bool tracksElementInitialization = false;
};

struct CapabilityFacts {
    bool hostOnly = false;
    bool runtimeRetained = false;
    bool ffi = false;
    bool gpu = false;
    bool maySuspend = false;
};

struct AbiFacts {
    bool stableBoundary = false;
    bool persistentFrameRequired = false;
    std::string dropGlueSymbol;
};

struct Facts {
    uint16_t schemaMajor = SchemaMajor;
    uint16_t schemaMinor = SchemaMinor;
    IdentityFacts identity;
    ControlFacts control;
    ResourceFacts resource;
    CapabilityFacts capability;
    AbiFacts abi;
};

namespace detail {

inline void appendIdentityPart(std::string& output, const std::string& part) {
    output += std::to_string(part.size());
    output += ':';
    output += part;
    output += ';';
}

template <typename Enum>
inline void appendEnum(std::string& output, Enum value) {
    appendIdentityPart(
        output, std::to_string(static_cast<unsigned>(value)));
}

inline void appendBool(std::string& output, bool value) {
    appendIdentityPart(output, value ? "1" : "0");
}

inline void appendOwnershipContract(
    std::string& output, const luna::ownership::Contract& contract) {
    appendEnum(output, contract.relation);
    appendEnum(output, contract.usage);
}

} // namespace detail

// One canonical declaration-contract encoder is shared by the frontend Symbol
// Catalog and sealed MoonIR. Keeping both the kind and encoder in core prevents
// either layer from inventing a parallel ContractId rule.
inline std::string canonicalDeclarationContract(
    DeclarationKind declarationKind,
    const luna::identity::TypeId& type,
    const Facts& facts,
    const luna::identity::SymbolId& dropGlueSymbol = {},
    const luna::identity::ContractId& dropGlueContract = {}) {
    std::string result = "luna.contract.v1;";
    detail::appendEnum(result, declarationKind);
    detail::appendIdentityPart(result, type.value);

    detail::appendEnum(result, facts.control.form);
    detail::appendEnum(result, facts.control.cardinality);
    detail::appendEnum(result, facts.control.storage);
    detail::appendEnum(result, facts.control.forwarding);
    detail::appendBool(result, facts.control.abortPermitted);
    detail::appendBool(result, facts.control.replayValidated);

    detail::appendIdentityPart(
        result, std::to_string(facts.resource.parameters.size()));
    for (const auto& parameter : facts.resource.parameters)
        detail::appendOwnershipContract(result, parameter);
    detail::appendOwnershipContract(result, facts.resource.result);
    detail::appendEnum(result, facts.resource.management);
    detail::appendEnum(result, facts.resource.releaseDomain);
    detail::appendEnum(result, facts.resource.lifetime);
    detail::appendEnum(result, facts.resource.relation);
    detail::appendEnum(result, facts.resource.usage);
    detail::appendEnum(result, facts.resource.cleanup);
    detail::appendBool(result, facts.resource.cleanupRequired);
    detail::appendBool(result, facts.resource.recursiveCleanup);
    detail::appendBool(result, facts.resource.needsDrop);
    detail::appendBool(result, facts.resource.tracksElementInitialization);

    detail::appendBool(result, facts.capability.hostOnly);
    detail::appendBool(result, facts.capability.runtimeRetained);
    detail::appendBool(result, facts.capability.ffi);
    detail::appendBool(result, facts.capability.gpu);
    detail::appendBool(result, facts.capability.maySuspend);
    detail::appendBool(result, facts.abi.stableBoundary);
    detail::appendBool(result, facts.abi.persistentFrameRequired);
    detail::appendIdentityPart(result, dropGlueSymbol.value);
    detail::appendIdentityPart(result, dropGlueContract.value);
    return result;
}

inline constexpr const char* controlFormName(ControlForm form) {
    switch (form) {
        case ControlForm::Plain: return "plain";
        case ControlForm::Interceptor: return "interceptor";
        case ControlForm::Context: return "context";
        case ControlForm::Coroutine: return "coroutine";
    }
    return "invalid";
}

inline constexpr const char* cardinalityName(Cardinality cardinality) {
    switch (cardinality) {
        case Cardinality::None: return "none";
        case Cardinality::Once: return "once";
        case Cardinality::Many: return "many";
    }
    return "invalid";
}

inline constexpr const char* continuationStorageName(
    ContinuationStorage storage) {
    switch (storage) {
        case ContinuationStorage::None: return "none";
        case ContinuationStorage::ScopedStack: return "scoped_stack";
        case ContinuationStorage::PersistentFrame: return "persistent_frame";
    }
    return "invalid";
}

inline constexpr const char* releaseDomainName(ReleaseDomain domain) {
    switch (domain) {
        case ReleaseDomain::None: return "none";
        case ReleaseDomain::LunaGlobal: return "luna_global";
        case ReleaseDomain::Foreign: return "foreign";
        case ReleaseDomain::Device: return "device";
        case ReleaseDomain::Executable: return "executable";
        case ReleaseDomain::HostService: return "host_service";
    }
    return "invalid";
}

inline constexpr const char* resourceLifetimeName(
    ResourceLifetime lifetime) {
    switch (lifetime) {
        case ResourceLifetime::Value: return "value";
        case ResourceLifetime::Lexical: return "lexical";
        case ResourceLifetime::Borrowed: return "borrowed";
        case ResourceLifetime::Explicit: return "explicit";
    }
    return "invalid";
}

inline constexpr const char* forwardingName(Forwarding forwarding) {
    switch (forwarding) {
        case Forwarding::None: return "none";
        case Forwarding::Automatic: return "automatic";
        case Forwarding::Explicit: return "explicit";
    }
    return "invalid";
}

inline constexpr const char* resourceManagementName(
    ResourceManagement management) {
    switch (management) {
        case ResourceManagement::Value: return "value";
        case ResourceManagement::Unique: return "unique";
    }
    return "invalid";
}

} // namespace luna::sysmeta
