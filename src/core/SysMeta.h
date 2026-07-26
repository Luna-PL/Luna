#pragma once

#include "Ownership.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luna::sysmeta {

// Sysmeta is compiler authority: source code may inspect stable projections
// in the future, but it can never construct, attach, or override these facts.
// Keep it typed so safety decisions never depend on user-controlled strings.
inline constexpr uint16_t SchemaMajor = 1;
inline constexpr uint16_t SchemaMinor = 0;
inline constexpr const char* DropTraitId = "luna.compiler.Drop";
inline constexpr const char* DropMethodName = "drop";
inline constexpr const char* OptionTypeId = "org.luna.core::prelude::Option";
inline constexpr const char* IteratorTraitId =
    "org.luna.core::iter::Iterator";
inline constexpr const char* IntoIteratorTraitId =
    "org.luna.core::iter::IntoIterator";
inline constexpr const char* FromIteratorTraitId =
    "org.luna.core::iter::FromIterator";

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
    bool needsDrop = false;
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
