#pragma once

// Canonical, target-independent type model shared by frontend and MoonIR.

#include "TypeIdentity.h"
#include "Ownership.h"
#include "SysMeta.h"

#include <memory>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cstdint>

using TypeVec = std::vector<TypePtr>;

enum class ContinuationKind { Interceptor, Context };

enum class TypeKind {
    I8, I16, I32, I64, U8, U16, U32, U64, USize, ISize,
    F32, F64, Bool, String, CStr, RawPointer, Unit, Never,
    Struct, Record, Enum, Result, Trait, TypeParam, Reference,
    Function, Closure, Slot, Fragment, Iterator,
    DeviceBuffer, Event, Array, Slice,
    Metadata, MetadataView, DeclarationView, DeclarationRef,
    InferenceVar, SymbolSet,
    Unknown
};

enum class IteratorMode : uint8_t {
    Copy,
    Shared,
    Mutable,
    Consuming,
    Range,
};

enum class IteratorOp : uint8_t {
    None,
    Iter,
    IterMut,
    IntoIter,
    Range,
    Map,
    Filter,
    Take,
    Fold,
    ForEach,
    Count,
    Collect,
};

struct TypeField {
    std::string name;
    TypePtr type;
};

struct TypeVariant {
    std::string name;
    TypeVec fields;
};

struct ResourceContract {
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::CleanupAction cleanup =
        luna::ownership::CleanupAction::None;
    luna::sysmeta::ResourceManagement management =
        luna::sysmeta::ResourceManagement::Value;
    luna::sysmeta::ReleaseDomain releaseDomain =
        luna::sysmeta::ReleaseDomain::None;
    luna::sysmeta::ResourceLifetime lifetime =
        luna::sysmeta::ResourceLifetime::Value;
    bool cleanupRequired = false;
    bool recursiveCleanup = false;
};

inline bool typeRequiresCleanup(const TypePtr& type);
inline luna::ownership::CleanupAction cleanupActionForType(
    const TypePtr& type);
inline ResourceContract resourceContractForType(const TypePtr& type);

struct Type {
    TypeKind kind;
    luna::types::TypeDomain domain = luna::types::TypeDomain::Value;
    luna::types::IdentityMode identityMode = luna::types::IdentityMode::Structural;
    std::string name;
    // Source declaration provenance used by tooling member identities. It is
    // deliberately excluded from language-level type equality. Named types
    // use nominalId; anonymous structural values use their canonical shape.
    std::string declarationLinkageName;
    // A non-empty nominalId is the identity of a declaration. Two named
    // structs with identical fields therefore remain incompatible. Record
    // types leave it empty and compare structurally.
    std::string nominalId;
    std::vector<std::string> typeParams;
    TypeVec typeArgs;

    TypePtr inner;                // for Reference, RawPointer, and DeviceBuffer
    uint64_t arrayLength = 0;     // for Array; part of the structural type
    bool isMutable = false;       // for Reference
    TypeVec paramTypes;          // for Function/Closure params
    TypePtr returnType;          // for Function/Closure return
    // Callable ownership is part of its language-level shape. A backend may
    // erase this from the machine ABI only after MoonIR verification.
    std::vector<luna::ownership::Contract> paramContracts;
    luna::ownership::Contract returnContract;
    // Canonical capture environment for Closure. Fields are kept in canonical
    // name order and participate in the structural type identity, value size,
    // and ABI layout (C016 CL002/CL004).
    std::vector<TypeField> capturedFields;
    // Compiler-derived, read-only semantic facts. This replaces a separate
    // user-visible effect summary without turning sysmeta into user metadata.
    luna::sysmeta::Facts sysmeta;
    // Slots and fragments are structural continuations. `isMultiShot` is part
    // of their type: a Many fragment cannot bind to a Once-only slot.
    bool isMultiShot = false;
    ContinuationKind continuationKind = ContinuationKind::Context;
    IteratorMode iteratorMode = IteratorMode::Copy;
    std::vector<TypeField> fields;       // for Struct/Record
    std::vector<TypeVariant> variants;   // for Enum
    int inferenceId = -1;        // for InferenceVar

    static TypePtr makePrimitive(TypeKind k) {
        auto t = std::make_shared<Type>();
        t->kind = k;
        t->identityMode = luna::types::IdentityMode::Builtin;
        if (k == TypeKind::String) {
            t->sysmeta.resource.management =
                luna::sysmeta::ResourceManagement::Unique;
            t->sysmeta.resource.releaseDomain =
                luna::sysmeta::ReleaseDomain::LunaGlobal;
        }
        return t;
    }
    static TypePtr makeStruct(const std::string& n,
                              std::vector<TypeField> fields = {},
                              std::string nominal = "") {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Struct; t->name = n;
        t->nominalId = std::move(nominal);
        t->identityMode = t->nominalId.empty()
            ? luna::types::IdentityMode::Structural
            : luna::types::IdentityMode::Nominal;
        t->fields = std::move(fields);
        t->sysmeta.resource.management =
            luna::sysmeta::ResourceManagement::Unique;
        t->sysmeta.resource.releaseDomain =
            luna::sysmeta::ReleaseDomain::LunaGlobal;
        return t;
    }
    static TypePtr makeRecord(std::vector<TypeField> fields) {
        std::sort(fields.begin(), fields.end(),
                  [](const TypeField& lhs, const TypeField& rhs) {
                      return lhs.name < rhs.name;
                  });
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Record;
        t->identityMode = luna::types::IdentityMode::Structural;
        t->fields = std::move(fields);
        return t;
    }
    static TypePtr makeEnum(const std::string& n,
                            std::vector<TypeVariant> variants = {},
                            std::string nominal = "") {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Enum; t->name = n;
        t->nominalId = std::move(nominal);
        t->identityMode = t->nominalId.empty()
            ? luna::types::IdentityMode::Structural
            : luna::types::IdentityMode::Nominal;
        t->variants = std::move(variants);
        return t;
    }
    static TypePtr makeResult(TypePtr value, TypePtr error) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Result;
        t->name = "Result";
        t->nominalId = luna::sysmeta::ResultTypeId;
        t->identityMode = luna::types::IdentityMode::Nominal;
        t->typeArgs = {std::move(value), std::move(error)};
        return t;
    }
    static TypePtr makeIterator(TypePtr item, IteratorMode mode,
                                TypePtr source = nullptr) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Iterator;
        t->name = "Iterator";
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        t->inner = std::move(item);
        t->iteratorMode = mode;
        // The initial adapter lowering is a host-only compiler recipe. This
        // is derived compiler authority, not an effect annotation supplied
        // by source code.
        t->sysmeta.capability.hostOnly = true;
        if (source) t->typeArgs.push_back(std::move(source));
        return t;
    }
    static TypePtr makeTrait(const std::string& n) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Trait; t->name = n;
        t->domain = luna::types::TypeDomain::Compiler;
        t->nominalId = n;
        t->identityMode = luna::types::IdentityMode::Nominal;
        return t;
    }
    static TypePtr makeTypeParam(const std::string& n) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::TypeParam; t->name = n;
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        return t;
    }
    static TypePtr makeReference(TypePtr inner, bool isMutable = false) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Reference; t->inner = inner; t->isMutable = isMutable;
        return t;
    }
    static TypePtr makeRawPointer(TypePtr inner) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::RawPointer; t->inner = std::move(inner);
        return t;
    }
    static TypePtr makeDeviceBuffer(TypePtr element) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::DeviceBuffer;
        t->inner = std::move(element);
        t->sysmeta.resource.management =
            luna::sysmeta::ResourceManagement::Unique;
        t->sysmeta.resource.releaseDomain =
            luna::sysmeta::ReleaseDomain::Device;
        return t;
    }
    static TypePtr makeArray(TypePtr element, uint64_t length) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Array; t->inner = std::move(element); t->arrayLength = length;
        return t;
    }
    static TypePtr makeSlice(TypePtr element) {
        auto t = std::make_shared<Type>(); t->kind = TypeKind::Slice; t->inner = std::move(element); return t;
    }
    static TypePtr makeEvent() {
        return makePrimitive(TypeKind::Event);
    }
    static TypePtr makeMetadata(const std::string& schemaName,
                                std::vector<TypeField> schemaFields = {}) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Metadata;
        t->domain = luna::types::TypeDomain::Meta;
        t->identityMode = luna::types::IdentityMode::MetaSchema;
        t->name = schemaName;
        t->nominalId = schemaName;
        t->fields = std::move(schemaFields);
        return t;
    }
    static TypePtr makeMetadataView(TypePtr metadata = nullptr) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::MetadataView;
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        t->inner = std::move(metadata);
        return t;
    }
    static TypePtr makeSymbolSet(TypePtr symbol = nullptr) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::SymbolSet;
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        t->inner = std::move(symbol);
        return t;
    }
    static TypePtr makeDeclarationView(TypePtr callable = nullptr) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::DeclarationView;
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        t->inner = std::move(callable);
        return t;
    }
    static TypePtr makeDeclarationRef(TypePtr callable = nullptr) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::DeclarationRef;
        t->domain = luna::types::TypeDomain::Compiler;
        t->identityMode = luna::types::IdentityMode::CompilerIntrinsic;
        t->inner = std::move(callable);
        return t;
    }
    static TypePtr makeCompileTimeOption(TypePtr value = nullptr) {
        auto t = makeEnum(
            "Option",
            {{"None", {}}, {"Some", {value}}},
            luna::sysmeta::OptionTypeId);
        t->domain = luna::types::TypeDomain::Compiler;
        t->typeArgs = {std::move(value)};
        return t;
    }
    static TypePtr makeFunction(
        TypeVec params, TypePtr ret,
        std::vector<luna::ownership::Contract> contracts = {},
        luna::ownership::Contract resultContract = {}) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Function;
        t->paramTypes = std::move(params);
        t->returnType = ret;
        t->paramContracts = std::move(contracts);
        if (t->paramContracts.empty())
            t->paramContracts.resize(t->paramTypes.size());
        t->returnContract = resultContract;
        t->sysmeta.resource.parameters = t->paramContracts;
        t->sysmeta.resource.result = t->returnContract;
        return t;
    }
    static TypePtr makeClosure(
        TypeVec params, TypePtr ret,
        std::vector<luna::ownership::Contract> contracts = {},
        luna::ownership::Contract resultContract = {},
        std::vector<TypeField> captures = {}) {
        std::sort(captures.begin(), captures.end(),
                  [](const TypeField& lhs, const TypeField& rhs) {
                      return lhs.name < rhs.name;
                  });
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Closure;
        t->paramTypes = std::move(params);
        t->returnType = std::move(ret);
        t->paramContracts = std::move(contracts);
        if (t->paramContracts.empty())
            t->paramContracts.resize(t->paramTypes.size());
        t->returnContract = resultContract;
        t->capturedFields = std::move(captures);
        t->sysmeta.resource.parameters = t->paramContracts;
        t->sysmeta.resource.result = t->returnContract;
        return t;
    }
    static TypePtr makeSlot(
        TypeVec params, TypePtr ret = nullptr, bool multiShot = false,
        ContinuationKind behavior = ContinuationKind::Context,
        std::vector<luna::ownership::Contract> contracts = {},
        luna::ownership::Contract resultContract = {}) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Slot;
        t->paramTypes = std::move(params);
        t->returnType = ret ? std::move(ret) : makePrimitive(TypeKind::Unit);
        t->paramContracts = std::move(contracts);
        if (t->paramContracts.empty())
            t->paramContracts.resize(t->paramTypes.size());
        t->returnContract = resultContract;
        t->isMultiShot = multiShot;
        t->continuationKind = behavior;
        t->sysmeta.resource.parameters = t->paramContracts;
        t->sysmeta.resource.result = t->returnContract;
        t->sysmeta.control.form =
            behavior == ContinuationKind::Interceptor
                ? luna::sysmeta::ControlForm::Interceptor
                : luna::sysmeta::ControlForm::Context;
        t->sysmeta.control.cardinality = multiShot
            ? luna::sysmeta::Cardinality::Many
            : luna::sysmeta::Cardinality::Once;
        t->sysmeta.control.storage =
            luna::sysmeta::ContinuationStorage::ScopedStack;
        t->sysmeta.control.forwarding =
            behavior == ContinuationKind::Interceptor
                ? luna::sysmeta::Forwarding::Automatic
                : luna::sysmeta::Forwarding::Explicit;
        t->sysmeta.control.abortPermitted = true;
        t->sysmeta.capability.hostOnly = true;
        return t;
    }
    static TypePtr makeFragment(
        TypeVec params, TypePtr ret = nullptr, bool multiShot = false,
        ContinuationKind behavior = ContinuationKind::Context,
        std::vector<luna::ownership::Contract> contracts = {},
        luna::ownership::Contract resultContract = {}) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Fragment;
        t->paramTypes = std::move(params);
        t->returnType = ret ? std::move(ret) : makePrimitive(TypeKind::Unit);
        t->paramContracts = std::move(contracts);
        if (t->paramContracts.empty())
            t->paramContracts.resize(t->paramTypes.size());
        t->returnContract = resultContract;
        t->isMultiShot = multiShot;
        t->continuationKind = behavior;
        t->sysmeta.resource.parameters = t->paramContracts;
        t->sysmeta.resource.result = t->returnContract;
        t->sysmeta.control.form =
            behavior == ContinuationKind::Interceptor
                ? luna::sysmeta::ControlForm::Interceptor
                : luna::sysmeta::ControlForm::Context;
        t->sysmeta.control.cardinality = multiShot
            ? luna::sysmeta::Cardinality::Many
            : luna::sysmeta::Cardinality::Once;
        t->sysmeta.control.storage =
            luna::sysmeta::ContinuationStorage::ScopedStack;
        t->sysmeta.control.forwarding =
            behavior == ContinuationKind::Interceptor
                ? luna::sysmeta::Forwarding::Automatic
                : luna::sysmeta::Forwarding::Explicit;
        t->sysmeta.control.abortPermitted = true;
        t->sysmeta.capability.hostOnly = true;
        return t;
    }
    static TypePtr makeUnknown() {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Unknown;
        t->domain = luna::types::TypeDomain::Error;
        t->identityMode = luna::types::IdentityMode::Error;
        return t;
    }
    static TypePtr makeInferenceVar(int id) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::InferenceVar;
        t->domain = luna::types::TypeDomain::Inference;
        t->identityMode = luna::types::IdentityMode::Inference;
        t->inferenceId = id;
        return t;
    }

    bool isHeapType() const {
        std::unordered_set<const Type*> active;
        std::function<bool(const Type*)> visit = [&](const Type* type) {
            if (!type || !active.insert(type).second) return false;
            if (type->sysmeta.resource.needsDrop) return true;
            if (type->kind == TypeKind::Result) {
                for (const auto& argument : type->typeArgs)
                    if (argument && visit(argument.get())) return true;
                return false;
            }
            if (type->kind == TypeKind::Enum) {
                for (const auto& variant : type->variants)
                    for (const auto& field : variant.fields)
                        if (field && visit(field.get())) return true;
                return false;
            }
            if (type->kind == TypeKind::Array)
                return visit(type->inner.get());
            if (type->kind == TypeKind::Record) {
                for (const auto& field : type->fields)
                    if (field.type && visit(field.type.get())) return true;
                return false;
            }
            if (type->kind == TypeKind::Closure) {
                for (const auto& field : type->capturedFields)
                    if (field.type && visit(field.type.get())) return true;
                return false;
            }
            return type->kind == TypeKind::String ||
                   type->kind == TypeKind::Struct ||
                   type->kind == TypeKind::DeviceBuffer;
        };
        return visit(this);
    }

    std::string toString() const {
        switch (kind) {
            case TypeKind::I8: return "i8";
            case TypeKind::I16: return "i16";
            case TypeKind::I32: return "i32";
            case TypeKind::I64: return "i64";
            case TypeKind::U8: return "u8";
            case TypeKind::U16: return "u16";
            case TypeKind::U32: return "u32";
            case TypeKind::U64: return "u64";
            case TypeKind::USize: return "usize";
            case TypeKind::ISize: return "isize";
            case TypeKind::F32: return "f32";
            case TypeKind::F64: return "f64";
            case TypeKind::Bool: return "bool";
            case TypeKind::String: return "string";
            case TypeKind::CStr: return "cstr";
            case TypeKind::RawPointer:
                return "raw<" + (inner ? inner->toString() : "?") + ">";
            case TypeKind::DeviceBuffer:
                return "device_buffer<" + (inner ? inner->toString() : "?") + ">";
            case TypeKind::Array:
                return "array<" + (inner ? inner->toString() : "?") + ", " +
                    std::to_string(arrayLength) + ">";
            case TypeKind::Slice: return "slice<" + (inner ? inner->toString() : "?") + ">";
            case TypeKind::Event: return "event";
            case TypeKind::Metadata: return "meta " + name;
            case TypeKind::MetadataView:
                return "metadata_view<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::SymbolSet:
                return "symbol_set<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::DeclarationView:
                return "declaration_view<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::DeclarationRef:
                return "declaration_ref<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::Unit: return "unit";
            case TypeKind::Never: return "never";
            case TypeKind::Record: {
                std::string result = "{";
                for (size_t index = 0; index < fields.size(); ++index) {
                    if (index) result += ", ";
                    result += fields[index].name + ": " +
                        (fields[index].type
                            ? fields[index].type->toString() : "?");
                }
                result += "}";
                return result;
            }
            case TypeKind::Struct:
            case TypeKind::Enum: {
                std::string result = name;
                if (!typeArgs.empty()) {
                    result += "<";
                    for (size_t i = 0; i < typeArgs.size(); ++i) {
                        if (i) result += ", ";
                        result += typeArgs[i]->toString();
                    }
                    result += ">";
                }
                return result;
            }
            case TypeKind::Result:
                return "Result<" +
                    (typeArgs.size() > 0 && typeArgs[0]
                        ? typeArgs[0]->toString() : "?") + ", " +
                    (typeArgs.size() > 1 && typeArgs[1]
                        ? typeArgs[1]->toString() : "?") + ">";
            case TypeKind::Trait: return "trait " + name;
            case TypeKind::TypeParam: return "'" + name;
            case TypeKind::Reference:
                return isMutable ? "&mut " + (inner ? inner->toString() : "?")
                                  : "&" + (inner ? inner->toString() : "?");
            case TypeKind::Function: return "fn(...)";
            case TypeKind::Closure: return "closure(...)";
            case TypeKind::Slot: return
                std::string(continuationKind == ContinuationKind::Interceptor
                                ? "slot interceptor(" : "slot context(") +
                std::to_string(paramTypes.size()) + (isMultiShot ? "; many)" : "; once)");
            case TypeKind::Fragment: return
                std::string(continuationKind == ContinuationKind::Interceptor
                                ? "interceptor(" : "context(") +
                std::to_string(paramTypes.size()) + (isMultiShot ? "; many)" : "; once)");
            case TypeKind::Iterator:
                return "iterator<" +
                    (inner ? inner->toString() : std::string("?")) + ">";
            case TypeKind::InferenceVar: return "?" + std::to_string(inferenceId);
            default: return "<unknown>";
        }
    }

};

inline luna::ownership::Usage defaultUsageForType(const TypePtr& type) {
    std::unordered_set<const Type*> active;
    std::function<luna::ownership::Usage(const TypePtr&)> visit =
        [&](const TypePtr& item) {
        if (!item) return luna::ownership::Usage::Copy;
        if (!active.insert(item.get()).second)
            return luna::ownership::Usage::Copy;
        if (item->kind == TypeKind::Result ||
            item->kind == TypeKind::Enum ||
            item->kind == TypeKind::Array ||
            item->kind == TypeKind::Record ||
            item->kind == TypeKind::Struct ||
            item->kind == TypeKind::Closure) {
            auto usage = item->kind == TypeKind::Struct ||
                    item->sysmeta.resource.needsDrop
                ? luna::ownership::Usage::Affine
                : luna::ownership::Usage::Copy;
            std::vector<TypePtr> payloads;
            if (item->kind == TypeKind::Result)
                payloads = item->typeArgs;
            else if (item->kind == TypeKind::Enum)
                for (const auto& variant : item->variants)
                    payloads.insert(
                        payloads.end(),
                        variant.fields.begin(), variant.fields.end());
            else if (item->kind == TypeKind::Record ||
                     item->kind == TypeKind::Struct)
                for (const auto& field : item->fields)
                    payloads.push_back(field.type);
            else if (item->kind == TypeKind::Closure)
                for (const auto& field : item->capturedFields)
                    payloads.push_back(field.type);
            else
                payloads.push_back(item->inner);
            for (const auto& payload : payloads) {
                const auto payloadUsage = visit(payload);
                if (payloadUsage == luna::ownership::Usage::Linear) {
                    active.erase(item.get());
                    return luna::ownership::Usage::Linear;
                }
                if (payloadUsage == luna::ownership::Usage::Affine)
                    usage = luna::ownership::Usage::Affine;
            }
            active.erase(item.get());
            return usage;
        }
        active.erase(item.get());
        if (item->sysmeta.resource.needsDrop)
            return luna::ownership::Usage::Affine;
        if (item->kind == TypeKind::Event ||
            item->kind == TypeKind::DeviceBuffer)
            return luna::ownership::Usage::Linear;
        if (item->kind == TypeKind::Iterator)
            return luna::ownership::Usage::Affine;
        if (item->isHeapType())
            return luna::ownership::Usage::Affine;
        return luna::ownership::Usage::Copy;
    };
    return visit(type);
}

inline bool typeRequiresCleanup(const TypePtr& type) {
    std::unordered_set<const Type*> active;
    std::function<bool(const TypePtr&)> visit =
        [&](const TypePtr& item) -> bool {
        if (!item || !active.insert(item.get()).second) return false;
        if (item->sysmeta.resource.needsDrop) {
            active.erase(item.get());
            return true;
        }
        switch (item->kind) {
            case TypeKind::String:
            case TypeKind::Struct:
            case TypeKind::DeviceBuffer:
                active.erase(item.get());
                return true;
            case TypeKind::Result:
                for (const auto& argument : item->typeArgs)
                    if (visit(argument)) {
                        active.erase(item.get());
                        return true;
                    }
                break;
            case TypeKind::Enum:
                for (const auto& variant : item->variants)
                    for (const auto& field : variant.fields)
                        if (visit(field)) {
                            active.erase(item.get());
                            return true;
                        }
                break;
            case TypeKind::Array:
                if (visit(item->inner)) {
                    active.erase(item.get());
                    return true;
                }
                break;
            case TypeKind::Record:
                for (const auto& field : item->fields)
                    if (visit(field.type)) {
                        active.erase(item.get());
                        return true;
                    }
                break;
            case TypeKind::Closure:
                for (const auto& field : item->capturedFields)
                    if (visit(field.type)) {
                        active.erase(item.get());
                        return true;
                    }
                break;
            default:
                break;
        }
        active.erase(item.get());
        return false;
    };
    return visit(type);
}

inline bool typeHasRecursiveCleanup(const TypePtr& type) {
    if (!type) return false;
    if (type->kind == TypeKind::Result)
        for (const auto& argument : type->typeArgs)
            if (typeRequiresCleanup(argument)) return true;
    if (type->kind == TypeKind::Enum)
        for (const auto& variant : type->variants)
            for (const auto& field : variant.fields)
                if (typeRequiresCleanup(field)) return true;
    if (type->kind == TypeKind::Array)
        return typeRequiresCleanup(type->inner);
    if (type->kind == TypeKind::Record ||
        type->kind == TypeKind::Struct)
        for (const auto& field : type->fields)
            if (typeRequiresCleanup(field.type)) return true;
    if (type->kind == TypeKind::Closure)
        for (const auto& field : type->capturedFields)
            if (typeRequiresCleanup(field.type)) return true;
    return false;
}

inline luna::ownership::CleanupAction cleanupActionForType(
    const TypePtr& type) {
    using luna::ownership::CleanupAction;
    // A cleanup obligation may come from an owning allocation even when the
    // stored value is otherwise Copy (for example `new i32`). In that case
    // the binding contract still releases the allocation directly.
    if (!type || !typeRequiresCleanup(type))
        return CleanupAction::Deallocate;
    if (type->kind == TypeKind::Result) return CleanupAction::ResultDrop;
    if (type->kind == TypeKind::Enum) return CleanupAction::EnumDrop;
    if (type->kind == TypeKind::Array) return CleanupAction::ArrayDrop;
    if (type->kind == TypeKind::Record) return CleanupAction::RecordDrop;
    if (type->kind == TypeKind::DeviceBuffer)
        return CleanupAction::DeviceRelease;
    return type->sysmeta.resource.needsDrop ||
            typeHasRecursiveCleanup(type)
        ? CleanupAction::Drop
        : CleanupAction::Deallocate;
}

inline ResourceContract resourceContractForType(const TypePtr& type) {
    ResourceContract contract;
    if (!type) return contract;
    const bool cleanupRequired = typeRequiresCleanup(type);
    contract.relation = type->kind == TypeKind::Reference
        ? (type->isMutable
            ? luna::ownership::Relation::MutableBorrow
            : luna::ownership::Relation::SharedBorrow)
        : luna::ownership::Relation::Owned;
    contract.usage = defaultUsageForType(type);
    contract.cleanup = cleanupRequired
        ? cleanupActionForType(type)
        : luna::ownership::CleanupAction::None;
    contract.management = type->sysmeta.resource.management;
    contract.releaseDomain = type->sysmeta.resource.releaseDomain;
    if (type->kind == TypeKind::Reference)
        contract.lifetime = luna::sysmeta::ResourceLifetime::Borrowed;
    else if (type->kind == TypeKind::Event ||
             type->kind == TypeKind::DeviceBuffer)
        contract.lifetime = luna::sysmeta::ResourceLifetime::Explicit;
    else if (cleanupRequired)
        contract.lifetime = luna::sysmeta::ResourceLifetime::Lexical;
    contract.cleanupRequired = cleanupRequired;
    contract.recursiveCleanup = typeHasRecursiveCleanup(type);
    return contract;
}

inline luna::ownership::Contract parameterContractFor(
    const TypePtr& type,
    luna::ownership::Usage explicitUsage = luna::ownership::Usage::Copy,
    bool hasExplicitUsage = false) {
    if (type && type->kind == TypeKind::Reference) {
        return {type->isMutable ? luna::ownership::Relation::MutableBorrow
                                : luna::ownership::Relation::SharedBorrow,
                luna::ownership::Usage::Copy};
    }
    const auto usage = hasExplicitUsage ? explicitUsage : defaultUsageForType(type);
    // Unqualified move-only parameters preserve Luna's existing view
    // semantics. An explicit affine/linear qualifier is the owning `take`.
    if (!hasExplicitUsage && luna::ownership::isMoveOnly(usage))
        return {luna::ownership::Relation::SharedBorrow,
                luna::ownership::Usage::Copy};
    return {luna::ownership::Relation::Owned, usage};
}

// Pre-made types
inline TypePtr TyI32    = Type::makePrimitive(TypeKind::I32);
inline TypePtr TyI64    = Type::makePrimitive(TypeKind::I64);
inline TypePtr TyI8     = Type::makePrimitive(TypeKind::I8);
inline TypePtr TyI16    = Type::makePrimitive(TypeKind::I16);
inline TypePtr TyU8     = Type::makePrimitive(TypeKind::U8);
inline TypePtr TyU16    = Type::makePrimitive(TypeKind::U16);
inline TypePtr TyU32    = Type::makePrimitive(TypeKind::U32);
inline TypePtr TyU64    = Type::makePrimitive(TypeKind::U64);
inline TypePtr TyUSize  = Type::makePrimitive(TypeKind::USize);
inline TypePtr TyISize  = Type::makePrimitive(TypeKind::ISize);
inline TypePtr TyF32    = Type::makePrimitive(TypeKind::F32);
inline TypePtr TyF64    = Type::makePrimitive(TypeKind::F64);
inline TypePtr TyBool   = Type::makePrimitive(TypeKind::Bool);
inline TypePtr TyString = Type::makePrimitive(TypeKind::String);
inline TypePtr TyCStr   = Type::makePrimitive(TypeKind::CStr);
inline TypePtr TyUnit   = Type::makePrimitive(TypeKind::Unit);
inline TypePtr TyNever  = Type::makePrimitive(TypeKind::Never);
inline TypePtr TyEvent  = Type::makeEvent();
inline TypePtr TyUnknown = Type::makeUnknown();

// Resolve a TypeAST to a canonical Type
TypePtr resolveType(const struct TypeAST* ast,
                    const std::unordered_map<std::string, TypePtr>& typeBindings);

// Check if a type is numeric (i32, i64, f32, f64)
inline bool isNumericType(const TypePtr& t) {
    return t && (t->kind == TypeKind::I8 || t->kind == TypeKind::I16 ||
                 t->kind == TypeKind::I32 || t->kind == TypeKind::I64 ||
                 t->kind == TypeKind::U8 || t->kind == TypeKind::U16 ||
                 t->kind == TypeKind::U32 || t->kind == TypeKind::U64 ||
                 t->kind == TypeKind::USize || t->kind == TypeKind::ISize ||
                 t->kind == TypeKind::F32 || t->kind == TypeKind::F64);
}

inline bool isIntegerType(const TypePtr& t) {
    return t && (t->kind == TypeKind::I8 || t->kind == TypeKind::I16 ||
                 t->kind == TypeKind::I32 || t->kind == TypeKind::I64 ||
                 t->kind == TypeKind::U8 || t->kind == TypeKind::U16 ||
                 t->kind == TypeKind::U32 || t->kind == TypeKind::U64 ||
                 t->kind == TypeKind::USize || t->kind == TypeKind::ISize);
}
