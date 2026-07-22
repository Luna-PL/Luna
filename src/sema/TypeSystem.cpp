#include "TypeSystem.h"
#include "../parser/AST.h"

TypePtr resolveType(const TypeAST* ast,
                    const std::unordered_map<std::string, TypePtr>& typeBindings) {
    if (!ast) return TyUnit;

    auto named = dynamic_cast<const NamedTypeAST*>(ast);
    if (named) {
        auto it = typeBindings.find(named->name);
        if (it != typeBindings.end()) return it->second;

        if (named->name == "i32") return TyI32;
        if (named->name == "i64") return TyI64;
        if (named->name == "i8") return TyI8;
        if (named->name == "i16") return TyI16;
        if (named->name == "u8") return TyU8;
        if (named->name == "u16") return TyU16;
        if (named->name == "u32") return TyU32;
        if (named->name == "u64") return TyU64;
        if (named->name == "usize") return TyUSize;
        if (named->name == "isize") return TyISize;
        if (named->name == "f32") return TyF32;
        if (named->name == "f64") return TyF64;
        if (named->name == "bool") return TyBool;
        if (named->name == "string") return TyString;
        if (named->name == "cstr") return TyCStr;
        if (named->name == "unit") return TyUnit;
        if (named->name == "Self") return Type::makeTypeParam("Self");

        if (named->name == "raw") {
            if (named->typeArgs.size() != 1) return TyUnknown;
            return Type::makeRawPointer(resolveType(named->typeArgs[0].get(), typeBindings));
        }
        if (named->name == "device_buffer") {
            if (named->typeArgs.size() != 1) return TyUnknown;
            return Type::makeDeviceBuffer(resolveType(named->typeArgs[0].get(), typeBindings));
        }
        if (named->name == "array") {
            if (named->typeArgs.size() != 1 || !named->arrayLength) return TyUnknown;
            return Type::makeArray(resolveType(named->typeArgs[0].get(), typeBindings),
                                   *named->arrayLength);
        }
        if (named->name == "slice") {
            if (named->typeArgs.size() != 1) return TyUnknown;
            return Type::makeSlice(resolveType(named->typeArgs[0].get(), typeBindings));
        }
        if (named->name == "event") return TyEvent;
        if (named->name == "declaration_view") {
            TypePtr callable;
            if (!named->typeArgs.empty())
                callable = resolveType(named->typeArgs.front().get(), typeBindings);
            return Type::makeDeclarationView(callable);
        }
        if (named->name == "declaration_ref") {
            TypePtr callable;
            if (!named->typeArgs.empty())
                callable = resolveType(named->typeArgs.front().get(), typeBindings);
            return Type::makeDeclarationRef(callable);
        }

        auto type = Type::makeStruct(named->name);
        for (auto& arg : named->typeArgs)
            type->typeArgs.push_back(resolveType(arg.get(), typeBindings));
        return type;
    }

    auto ref = dynamic_cast<const RefTypeAST*>(ast);
    if (ref) {
        return Type::makeReference(resolveType(ref->inner.get(), typeBindings),
                                   ref->isMutable);
    }

    auto linear = dynamic_cast<const LinearTypeAST*>(ast);
    if (linear) return resolveType(linear->inner.get(), typeBindings);
    auto affine = dynamic_cast<const AffineTypeAST*>(ast);
    if (affine) return resolveType(affine->inner.get(), typeBindings);

    auto func = dynamic_cast<const FunctionTypeAST*>(ast);
    if (func) {
        TypeVec paramTypes;
        for (auto& pt : func->paramTypes) {
            paramTypes.push_back(resolveType(pt.get(), typeBindings));
        }
        TypePtr retType = resolveType(func->returnType.get(), typeBindings);
        return Type::makeFunction(std::move(paramTypes), retType);
    }

    return TyUnknown;
}

TypePtr ConstraintSolver::fresh() {
    return Type::makeInferenceVar(mNextId++);
}

TypePtr ConstraintSolver::resolve(const TypePtr& type) {
    if (!type) return TyUnknown;
    if (type->kind == TypeKind::InferenceVar) {
        auto it = mBindings.find(type->inferenceId);
        if (it == mBindings.end()) return type;
        TypePtr resolved = resolve(it->second);
        it->second = resolved;
        return resolved;
    }

    if ((type->kind == TypeKind::Reference || type->kind == TypeKind::RawPointer ||
         type->kind == TypeKind::DeviceBuffer || type->kind == TypeKind::Array ||
         type->kind == TypeKind::Slice || type->kind == TypeKind::DeclarationView ||
         type->kind == TypeKind::DeclarationRef) && type->inner)
        type->inner = resolve(type->inner);
    else if (type->kind == TypeKind::Function || type->kind == TypeKind::Slot ||
             type->kind == TypeKind::Fragment) {
        for (auto& param : type->paramTypes) param = resolve(param);
        type->returnType = resolve(type->returnType);
    } else if (type->kind == TypeKind::Record || type->kind == TypeKind::Struct) {
        for (auto& field : type->fields) field.type = resolve(field.type);
    } else if (type->kind == TypeKind::Enum) {
        for (auto& variant : type->variants)
            for (auto& field : variant.fields) field = resolve(field);
    }
    for (auto& arg : type->typeArgs) arg = resolve(arg);
    return type;
}

bool ConstraintSolver::contains(const TypePtr& type, int id) {
    auto resolved = resolve(type);
    if (!resolved) return false;
    if (resolved->kind == TypeKind::InferenceVar)
        return resolved->inferenceId == id;
    if (resolved->kind == TypeKind::Reference || resolved->kind == TypeKind::RawPointer ||
        resolved->kind == TypeKind::DeviceBuffer || resolved->kind == TypeKind::Array ||
        resolved->kind == TypeKind::DeclarationView || resolved->kind == TypeKind::DeclarationRef)
        return contains(resolved->inner, id);
    if (resolved->kind == TypeKind::Function || resolved->kind == TypeKind::Slot ||
        resolved->kind == TypeKind::Fragment) {
        for (auto& p : resolved->paramTypes)
            if (contains(p, id)) return true;
        return contains(resolved->returnType, id);
    }
    if (resolved->kind == TypeKind::Record) {
        for (auto& field : resolved->fields)
            if (contains(field.type, id)) return true;
    }
    for (auto& arg : resolved->typeArgs)
        if (contains(arg, id)) return true;
    return false;
}

bool ConstraintSolver::unifyInternal(const TypePtr& lhs, const TypePtr& rhs,
                                     std::string* reason) {
    auto a = resolve(lhs);
    auto b = resolve(rhs);
    if (!a || !b) return true;
    if (a->kind == TypeKind::Unknown || b->kind == TypeKind::Unknown) return true;
    if (a->kind == TypeKind::InferenceVar) {
        if (b->kind == TypeKind::InferenceVar &&
            a->inferenceId == b->inferenceId) return true;
        if (contains(b, a->inferenceId)) {
            if (reason) *reason = "recursive type constraint";
            return false;
        }
        if (mNumericConstraints[a->inferenceId] &&
            b->kind != TypeKind::InferenceVar && !isNumericType(b)) {
            if (reason) *reason = b->toString() + " is not numeric";
            return false;
        }
        if (mBoolConstraints[a->inferenceId] &&
            b->kind != TypeKind::InferenceVar && b->kind != TypeKind::Bool) {
            if (reason) *reason = b->toString() + " is not bool";
            return false;
        }
        if (b->kind == TypeKind::InferenceVar) {
            mNumericConstraints[b->inferenceId] =
                mNumericConstraints[b->inferenceId] || mNumericConstraints[a->inferenceId];
            mBoolConstraints[b->inferenceId] =
                mBoolConstraints[b->inferenceId] || mBoolConstraints[a->inferenceId];
        }
        mBindings[a->inferenceId] = b;
        return true;
    }
    if (b->kind == TypeKind::InferenceVar)
        return unifyInternal(b, a, reason);

    const bool bothProducts =
        (a->kind == TypeKind::Struct || a->kind == TypeKind::Record) &&
        (b->kind == TypeKind::Struct || b->kind == TypeKind::Record);
    if (a->domain != b->domain || (a->kind != b->kind && !bothProducts)) {
        if (reason) *reason = a->toString() + " and " + b->toString() + " are different types";
        return false;
    }
    const bool identityBearing =
        a->identityMode == luna::types::IdentityMode::Nominal ||
        a->identityMode == luna::types::IdentityMode::MetaSchema ||
        b->identityMode == luna::types::IdentityMode::Nominal ||
        b->identityMode == luna::types::IdentityMode::MetaSchema;
    if (identityBearing) {
        if (a->identityMode != b->identityMode || a->nominalId != b->nominalId) {
            if (reason) *reason = a->toString() + " and " + b->toString() +
                                  " are different types";
            return false;
        }
        if (a->typeArgs.size() != b->typeArgs.size()) {
            if (reason) *reason = "generic arguments differ";
            return false;
        }
        for (size_t i = 0; i < a->typeArgs.size(); ++i)
            if (!unifyInternal(a->typeArgs[i], b->typeArgs[i], reason)) return false;
        return true;
    }
    if ((a->kind == TypeKind::Trait || a->kind == TypeKind::TypeParam) &&
        a->name != b->name) {
        if (reason) *reason = a->toString() + " and " + b->toString() + " are different types";
        return false;
    }
    if (a->kind == TypeKind::Reference) {
        if (a->isMutable != b->isMutable) {
            if (reason) *reason = "mutable and shared references are different types";
            return false;
        }
        return unifyInternal(a->inner, b->inner, reason);
    }
    if (a->kind == TypeKind::RawPointer || a->kind == TypeKind::DeviceBuffer ||
        a->kind == TypeKind::DeclarationView || a->kind == TypeKind::DeclarationRef) {
        if (!a->inner || !b->inner) return !a->inner && !b->inner;
        return unifyInternal(a->inner, b->inner, reason);
    }
    if (bothProducts) {
        if (a->fields.size() != b->fields.size()) {
            if (reason) *reason = "record field counts differ";
            return false;
        }
        for (size_t i = 0; i < a->fields.size(); ++i) {
            if (a->fields[i].name != b->fields[i].name ||
                !unifyInternal(a->fields[i].type, b->fields[i].type, reason)) {
                if (reason && reason->empty()) *reason = "record fields differ";
                return false;
            }
        }
        return true;
    }
    if (a->kind == TypeKind::Enum) {
        if (a->variants.size() != b->variants.size()) {
            if (reason) *reason = "enum variant counts differ";
            return false;
        }
        for (size_t variant = 0; variant < a->variants.size(); ++variant) {
            if (a->variants[variant].name != b->variants[variant].name ||
                a->variants[variant].fields.size() !=
                    b->variants[variant].fields.size()) {
                if (reason) *reason = "enum variants differ";
                return false;
            }
            for (size_t field = 0;
                 field < a->variants[variant].fields.size(); ++field) {
                if (!unifyInternal(a->variants[variant].fields[field],
                                   b->variants[variant].fields[field], reason))
                    return false;
            }
        }
        return true;
    }
    if (a->kind == TypeKind::Function || a->kind == TypeKind::Slot ||
        a->kind == TypeKind::Fragment) {
        if ((a->kind == TypeKind::Fragment || a->kind == TypeKind::Slot) &&
            a->continuationKind != b->continuationKind) {
            if (reason) *reason = "interceptor and context are different continuation contracts";
            return false;
        }
        if (a->isMultiShot != b->isMultiShot) {
            if (reason) *reason = "continuation emission capabilities differ";
            return false;
        }
        if (a->paramTypes.size() != b->paramTypes.size()) {
            if (reason) *reason = "function arities differ";
            return false;
        }
        for (size_t i = 0; i < a->paramTypes.size(); ++i)
            if (!unifyInternal(a->paramTypes[i], b->paramTypes[i], reason)) return false;
        return unifyInternal(a->returnType, b->returnType, reason);
    }
    return true;
}

bool ConstraintSolver::unify(const TypePtr& lhs, const TypePtr& rhs,
                             std::string* reason) {
    return unifyInternal(lhs, rhs, reason);
}

void ConstraintSolver::requireNumeric(const TypePtr& type) {
    auto resolved = resolve(type);
    if (resolved && resolved->kind == TypeKind::InferenceVar) {
        mNumericConstraints[resolved->inferenceId] = true;
        return;
    }
    // Concrete numeric validation is performed by semantic analysis so it can
    // attach a useful source-level diagnostic.
}

void ConstraintSolver::requireBool(const TypePtr& type) {
    auto resolved = resolve(type);
    if (resolved && resolved->kind == TypeKind::InferenceVar)
        mBoolConstraints[resolved->inferenceId] = true;
}

void ConstraintSolver::collectUnresolvedNumeric(const TypePtr& type) {
    auto resolved = resolve(type);
    if (!resolved) return;
    if (resolved->kind == TypeKind::InferenceVar &&
        mNumericConstraints[resolved->inferenceId]) {
        mBindings[resolved->inferenceId] = TyI32;
        return;
    }
    if (resolved->kind == TypeKind::Reference || resolved->kind == TypeKind::RawPointer ||
        resolved->kind == TypeKind::DeclarationView ||
        resolved->kind == TypeKind::DeclarationRef)
        collectUnresolvedNumeric(resolved->inner);
    if (resolved->kind == TypeKind::Function || resolved->kind == TypeKind::Slot ||
        resolved->kind == TypeKind::Fragment) {
        for (auto& p : resolved->paramTypes) collectUnresolvedNumeric(p);
        collectUnresolvedNumeric(resolved->returnType);
    }
}

void ConstraintSolver::defaultUnconstrainedNumeric() {
    for (int i = 0; i < mNextId; ++i) {
        auto var = Type::makeInferenceVar(i);
        collectUnresolvedNumeric(var);
    }
}

bool ConstraintSolver::hasUnresolved(const TypePtr& type) {
    auto resolved = resolve(type);
    if (!resolved) return false;
    if (resolved->kind == TypeKind::InferenceVar) return true;
    if (resolved->kind == TypeKind::Reference || resolved->kind == TypeKind::RawPointer ||
        resolved->kind == TypeKind::DeclarationView ||
        resolved->kind == TypeKind::DeclarationRef)
        return hasUnresolved(resolved->inner);
    if (resolved->kind == TypeKind::Function || resolved->kind == TypeKind::Slot ||
        resolved->kind == TypeKind::Fragment) {
        for (auto& p : resolved->paramTypes)
            if (hasUnresolved(p)) return true;
        return hasUnresolved(resolved->returnType);
    }
    if (resolved->kind == TypeKind::Record || resolved->kind == TypeKind::Struct) {
        for (auto& field : resolved->fields)
            if (hasUnresolved(field.type)) return true;
    }
    if (resolved->kind == TypeKind::Enum) {
        for (auto& variant : resolved->variants)
            for (auto& field : variant.fields)
                if (hasUnresolved(field)) return true;
    }
    for (auto& arg : resolved->typeArgs)
        if (hasUnresolved(arg)) return true;
    return false;
}
