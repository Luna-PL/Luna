#pragma once

#include "../lexer/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct Type;
using TypePtr = std::shared_ptr<Type>;
using TypeVec = std::vector<TypePtr>;

enum class ContinuationKind { Interceptor, Context };

enum class TypeKind {
    I8, I16, I32, I64, U8, U16, U32, U64, USize, ISize,
    F32, F64, Bool, String, CStr, RawPointer, Unit,
    Struct, Record, Enum, Trait, TypeParam, Reference, Function, Slot, Fragment,
    DeviceBuffer, Event, Array, Slice,
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
        return t;
    }
    static TypePtr makeStruct(const std::string& n,
                              std::vector<TypeField> fields = {},
                              std::string nominal = "") {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Struct; t->name = n;
        t->nominalId = nominal.empty() ? n : std::move(nominal);
        t->fields = std::move(fields);
        return t;
    }
    static TypePtr makeRecord(std::vector<TypeField> fields) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Record;
        t->fields = std::move(fields);
        return t;
    }
    static TypePtr makeEnum(const std::string& n,
                            std::vector<TypeVariant> variants = {},
                            std::string nominal = "") {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Enum; t->name = n;
        t->nominalId = nominal.empty() ? n : std::move(nominal);
        t->variants = std::move(variants);
        return t;
    }
    static TypePtr makeTrait(const std::string& n) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Trait; t->name = n;
        return t;
    }
    static TypePtr makeTypeParam(const std::string& n) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::TypeParam; t->name = n;
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
    static TypePtr makeFunction(TypeVec params, TypePtr ret) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Function;
        t->paramTypes = std::move(params);
        t->returnType = ret;
        return t;
    }
    static TypePtr makeSlot(TypeVec params, TypePtr ret = nullptr, bool multiShot = false,
                            ContinuationKind behavior = ContinuationKind::Context) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Slot;
        t->paramTypes = std::move(params);
        t->returnType = ret ? std::move(ret) : makePrimitive(TypeKind::Unit);
        t->isMultiShot = multiShot;
        t->continuationKind = behavior;
        return t;
    }
    static TypePtr makeFragment(TypeVec params, TypePtr ret = nullptr, bool multiShot = false,
                                ContinuationKind behavior = ContinuationKind::Context) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Fragment;
        t->paramTypes = std::move(params);
        t->returnType = ret ? std::move(ret) : makePrimitive(TypeKind::Unit);
        t->isMultiShot = multiShot;
        t->continuationKind = behavior;
        return t;
    }
    static TypePtr makeUnknown() {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Unknown;
        return t;
    }
    static TypePtr makeInferenceVar(int id) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::InferenceVar;
        t->inferenceId = id;
        return t;
    }

    bool isHeapType() const {
        return kind == TypeKind::String || kind == TypeKind::Struct ||
               kind == TypeKind::Record || kind == TypeKind::Enum ||
               kind == TypeKind::DeviceBuffer;
    }

    static TypePtr fromTokenKind(TokenKind k) {
        switch (k) {
            case TokenKind::TyI32:  return makePrimitive(TypeKind::I32);
            case TokenKind::TyI64:  return makePrimitive(TypeKind::I64);
            case TokenKind::TyF32:  return makePrimitive(TypeKind::F32);
            case TokenKind::TyF64:  return makePrimitive(TypeKind::F64);
            case TokenKind::TyBool: return makePrimitive(TypeKind::Bool);
            case TokenKind::TyString: return makePrimitive(TypeKind::String);
            default: return makeUnknown();
        }
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

    bool equals(const TypePtr& other) const {
        if (!other) return false;
        if (kind != other->kind) return false;
        if (kind == TypeKind::Struct || kind == TypeKind::Enum) {
            if (nominalId != other->nominalId) return false;
            if (typeArgs.size() != other->typeArgs.size()) return false;
            for (size_t i = 0; i < typeArgs.size(); ++i)
                if (!typeArgs[i]->equals(other->typeArgs[i])) return false;
            return true;
        }
        if (kind == TypeKind::Trait || kind == TypeKind::TypeParam)
            return name == other->name;
        if (kind == TypeKind::Record) {
            if (fields.size() != other->fields.size()) return false;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].name != other->fields[i].name ||
                    !fields[i].type->equals(other->fields[i].type)) return false;
            }
            return true;
        }
        if (kind == TypeKind::InferenceVar)
            return inferenceId == other->inferenceId;
        if (kind == TypeKind::Reference)
            return isMutable == other->isMutable && inner && other->inner &&
                   inner->equals(other->inner);
        if (kind == TypeKind::RawPointer || kind == TypeKind::DeviceBuffer)
            return inner && other->inner && inner->equals(other->inner);
        if (kind == TypeKind::Array)
            return arrayLength == other->arrayLength && inner && other->inner &&
                   inner->equals(other->inner);
        if (kind == TypeKind::Slice) return inner && other->inner && inner->equals(other->inner);
        if (kind == TypeKind::Function || kind == TypeKind::Slot || kind == TypeKind::Fragment) {
            if (paramTypes.size() != other->paramTypes.size()) return false;
            for (size_t i = 0; i < paramTypes.size(); ++i)
                if (!paramTypes[i]->equals(other->paramTypes[i])) return false;
            if ((kind == TypeKind::Fragment || kind == TypeKind::Slot) &&
                continuationKind != other->continuationKind)
                return false;
            return isMultiShot == other->isMultiShot && returnType && other->returnType &&
                   returnType->equals(other->returnType);
        }
        return true;
    }
};

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

// A small Hindley-Milner-style constraint solver used by semantic analysis.
// Inference variables are deliberately kept separate from Unknown: Unknown is
// an error-recovery type, while an inference variable is a real type waiting
// for constraints from declarations and consumers.
class ConstraintSolver {
public:
    TypePtr fresh();
    TypePtr resolve(const TypePtr& type);
    bool unify(const TypePtr& lhs, const TypePtr& rhs, std::string* reason = nullptr);
    void requireNumeric(const TypePtr& type);
    void requireBool(const TypePtr& type);
    void defaultUnconstrainedNumeric();
    bool hasUnresolved(const TypePtr& type);

private:
    bool unifyInternal(const TypePtr& lhs, const TypePtr& rhs, std::string* reason);
    bool contains(const TypePtr& type, int id);
    void collectUnresolvedNumeric(const TypePtr& type);

    int mNextId = 0;
    std::unordered_map<int, TypePtr> mBindings;
    std::unordered_map<int, bool> mNumericConstraints;
    std::unordered_map<int, bool> mBoolConstraints;
};

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
