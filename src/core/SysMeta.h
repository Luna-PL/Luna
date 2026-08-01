#pragma once

#include "CoreContracts.h"
#include "Ownership.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luna::sysmeta {

// Sysmeta is compiler authority: source code may inspect stable projections
// in the future, but it can never construct, attach, or override these facts.
// Keep it typed so safety decisions never depend on user-controlled strings.
inline constexpr uint16_t SchemaMajor = 1;
inline constexpr uint16_t SchemaMinor = 1;
inline constexpr const char* DropTraitId =
    luna::core_contracts::legacy_0_2::DropTraitId;
inline constexpr const char* DropMethodName =
    luna::core_contracts::DropMethodName;
inline constexpr const char* FromTraitId =
    luna::core_contracts::legacy_0_2::FromTraitId;
inline constexpr const char* FromMethodName =
    luna::core_contracts::FromMethodName;
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

// These are recorded now so a future explicit 0.3 mode can switch identity
// without inventing another spelling or silently changing 0.2 packages.
inline constexpr const char* CanonicalDropTraitId =
    luna::core_contracts::canonical_0_3::DropTraitId;
inline constexpr const char* CanonicalFromTraitId =
    luna::core_contracts::canonical_0_3::FromTraitId;
inline constexpr const char* CanonicalResultTypeId =
    luna::core_contracts::canonical_0_3::ResultTypeId;

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
    Rc,
    Arc,
};

enum class ReleaseDomain : uint8_t {
    None,
    LunaGlobal,
    Foreign,
    Device,
    Executable,
    HostService,
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
    bool needsDrop = false;
    bool tracksElementInitialization = false;
};

struct CapabilityFacts {
    bool hostOnly = false;
    bool runtimeRetained = false;
    bool dynamicDispatch = false;
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
    ControlFacts control;
    ResourceFacts resource;
    CapabilityFacts capability;
    AbiFacts abi;
};

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
        case ResourceManagement::Rc: return "rc";
        case ResourceManagement::Arc: return "arc";
    }
    return "invalid";
}

} // namespace luna::sysmeta
