#pragma once

// Canonical, target-independent type model shared by frontend and MoonIR.

#include "TypeIdentity.h"
#include "Ownership.h"
#include "SysMeta.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

using TypeVec = std::vector<TypePtr>;

enum class ContinuationKind { Interceptor, Context };

enum class TypeKind {
    I8, I16, I32, I64, U8, U16, U32, U64, USize, ISize,
    F32, F64, Bool, String, CStr, RawPointer, Unit,
    Struct, Record, Enum, Result, Trait, TypeParam, Reference, Rc, Arc,
    Function, Slot, Fragment,
    DeviceBuffer, Event, Array, Slice,
    Metadata, MetadataView, DeclarationView, DeclarationRef,
    InferenceVar,
    Unknown
};

struct TypeField {
    std::string name;
    TypePtr type;
};

struct TypeVariant {
    std::string name;
    TypeVec fields;
};

struct Type {
    TypeKind kind;
    luna::types::TypeDomain domain = luna::types::TypeDomain::Value;
    luna::types::IdentityMode identityMode = luna::types::IdentityMode::Structural;
    std::string name;
    // A non-empty nominalId is the identity of a declaration. Two named
    // structs with identical fields therefore remain incompatible. Record
    // types leave it empty and compare structurally.
    std::string nominalId;
    std::vector<std::string> typeParams;
    TypeVec typeArgs;

    TypePtr inner;                // for Reference, RawPointer, and DeviceBuffer
    uint64_t arrayLength = 0;     // for Array; part of the structural type
    bool isMutable = false;       // for Reference
    TypeVec paramTypes;          // for Function params
    TypePtr returnType;          // for Function return
    // Callable ownership is part of its language-level shape. A backend may
    // erase this from the machine ABI only after MoonIR verification.
    std::vector<luna::ownership::Contract> paramContracts;
    luna::ownership::Contract returnContract;
    // Compiler-derived, read-only semantic facts. This replaces a separate
    // user-visible effect summary without turning sysmeta into user metadata.
    luna::sysmeta::Facts sysmeta;
    // Slots and fragments are structural continuations. `isMultiShot` is part
    // of their type: a Many fragment cannot bind to a Once-only slot.
    bool isMultiShot = false;
    ContinuationKind continuationKind = ContinuationKind::Context;
    std::vector<TypeField> fields;       // for Struct/Record
    std::vector<TypeVariant> variants;   // for Enum
    int inferenceId = -1;        // for InferenceVar

    static TypePtr makePrimitive(TypeKind k) {
        auto t = std::make_shared<Type>();
        t->kind = k;
        t->identityMode = luna::types::IdentityMode::Builtin;
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
        return t;
    }
    static TypePtr makeRecord(std::vector<TypeField> fields) {
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
        t->typeArgs = {std::move(value), std::move(error)};
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
    static TypePtr makeRc(TypePtr inner) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Rc;
        t->inner = std::move(inner);
        t->sysmeta.resource.management =
            luna::sysmeta::ResourceManagement::Rc;
        return t;
    }
    static TypePtr makeArc(TypePtr inner) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Arc;
        t->inner = std::move(inner);
        t->sysmeta.resource.management =
            luna::sysmeta::ResourceManagement::Arc;
        return t;
    }
    static TypePtr makeDeviceBuffer(TypePtr element) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::DeviceBuffer;
        t->inner = std::move(element);
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
        if (kind == TypeKind::Result) {
            for (const auto& argument : typeArgs)
                if (argument && argument->isHeapType()) return true;
            return false;
        }
        return kind == TypeKind::String || kind == TypeKind::Struct ||
               kind == TypeKind::Record || kind == TypeKind::Enum ||
               kind == TypeKind::DeviceBuffer || kind == TypeKind::Rc ||
               kind == TypeKind::Arc;
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
            case TypeKind::Rc:
                return "rc<" + (inner ? inner->toString() : "?") + ">";
            case TypeKind::Arc:
                return "arc<" + (inner ? inner->toString() : "?") + ">";
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
            case TypeKind::DeclarationView:
                return "declaration_view<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::DeclarationRef:
                return "declaration_ref<" + (inner ? inner->toString() : "_") + ">";
            case TypeKind::Unit: return "unit";
            case TypeKind::Struct:
            case TypeKind::Record:
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
            case TypeKind::Slot: return
                std::string(continuationKind == ContinuationKind::Interceptor
                                ? "slot interceptor(" : "slot context(") +
                std::to_string(paramTypes.size()) + (isMultiShot ? "; many)" : "; once)");
            case TypeKind::Fragment: return
                std::string(continuationKind == ContinuationKind::Interceptor
                                ? "interceptor(" : "context(") +
                std::to_string(paramTypes.size()) + (isMultiShot ? "; many)" : "; once)");
            case TypeKind::InferenceVar: return "?" + std::to_string(inferenceId);
            default: return "<unknown>";
        }
    }

};

inline luna::ownership::Usage defaultUsageForType(const TypePtr& type) {
    if (!type) return luna::ownership::Usage::Copy;
    if (type->kind == TypeKind::Result) {
        // A Result owns exactly one active payload, so its usage is the
        // strongest usage of either variant. Scalar-only Results remain Copy;
        // resource-bearing Results are Affine, and a linear payload makes the
        // whole container Linear.
        auto usage = luna::ownership::Usage::Copy;
        for (const auto& argument : type->typeArgs) {
            const auto item = defaultUsageForType(argument);
            if (item == luna::ownership::Usage::Linear)
                return luna::ownership::Usage::Linear;
            if (item == luna::ownership::Usage::Affine)
                usage = luna::ownership::Usage::Affine;
        }
        return usage;
    }
    if (type->kind == TypeKind::Event || type->kind == TypeKind::DeviceBuffer)
        return luna::ownership::Usage::Linear;
    if (type->isHeapType()) return luna::ownership::Usage::Affine;
    return luna::ownership::Usage::Copy;
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
