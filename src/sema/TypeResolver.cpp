#include "TypeResolver.h"
#include "PredefinedTypes.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include <functional>
#include <limits>
#include <utility>

namespace {

unsigned integerWidth(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:
        case TypeKind::U8: return 8;
        case TypeKind::I16:
        case TypeKind::U16: return 16;
        case TypeKind::I32:
        case TypeKind::U32: return 32;
        case TypeKind::I64:
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize: return 64;
        default: return 0;
    }
}

bool integerLiteralFits(uint64_t magnitude, const TypePtr& type,
                        bool negative) {
    if (!type || !isIntegerType(type)) return true;
    const unsigned width = integerWidth(type->kind);
    if (width == 0) return true;
    if (isUnsignedIntegerType(type)) {
        if (negative && magnitude != 0) return false;
        const uint64_t maximum = width == 64
            ? std::numeric_limits<uint64_t>::max()
            : (uint64_t{1} << width) - 1;
        return magnitude <= maximum;
    }
    const uint64_t maximum = negative
        ? (uint64_t{1} << (width - 1))
        : (uint64_t{1} << (width - 1)) - 1;
    return magnitude <= maximum;
}

} // namespace

FunctionDecl* TypeResolver::findMatchingImpl(const std::string& traitName,
                                                  const std::string& typeName,
                                                  const std::string& methodName) {
    auto traitIt = mContext.mImpls.find(traitName);
    if (traitIt == mContext.mImpls.end()) return nullptr;
    auto typeIt = traitIt->second.find(typeName);
    if (typeIt == traitIt->second.end()) return nullptr;
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    return methodIt->second;
}

TypePtr TypeResolver::validateTypeFormation(
    const TypePtr& type, const TypeAST* source) {
    std::string reason;
    if (!luna::types::isWellFormedTypeDomain(type, &reason)) {
        if (!source || mReportedTypeFormationErrors.insert(source).second)
            mContext.error(
                reason,
                source ? source->line : 0,
                source ? source->col : 0);
        return type;
    }
    if (!luna::layout::valueLayoutFits(type) &&
        (!source || mReportedTypeFormationErrors.insert(source).second)) {
        mContext.error(
            "type '" + type->toString() +
                "' has a value layout that exceeds the 64-bit value-size limit",
            source ? source->line : 0,
            source ? source->col : 0);
    }
    return type;
}

TypePtr TypeResolver::resolveTypeAST(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return TyUnit;
    if (auto* record = dynamic_cast<const RecordTypeAST*>(ast)) {
        if (record->resolvedType) return record->resolvedType;
        std::vector<TypeField> fields;
        fields.reserve(record->fields.size());
        for (const auto& field : record->fields)
            fields.push_back({
                field.name, resolveTypeAST(field.type.get(), bindings)});
        auto resolvedRecord = validateTypeFormation(
            Type::makeRecord(std::move(fields)), record);
        const_cast<RecordTypeAST*>(record)->resolvedType = resolvedRecord;
        return resolvedRecord;
    }
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        // Monomorphization may materialize an already-resolved nominal type
        // into an AST annotation. Preserve that exact identity instead of
        // resolving its display name through the declaration family again.
        if (named->resolvedType) return named->resolvedType;
        auto predefined = resolvePredefinedType(
            *named, [&](const TypeAST* argument) {
                return resolveTypeAST(argument, bindings);
            });
        if (predefined.recognized) {
            if (!predefined.error.empty()) {
                mContext.error(predefined.error, named->line, named->col);
                return TyUnknown;
            }
            if (predefined.form == PredefinedTypeForm::MetadataView &&
                resolved(predefined.type->inner)->kind != TypeKind::Metadata)
                mContext.error("metadata_view type argument must be a meta schema",
                      named->line, named->col);
            return validateTypeFormation(predefined.type, named);
        }
        auto bound = bindings.find(named->name);
        if (bound != bindings.end()) return bound->second;
        if (auto* symbol = mContext.mSymTable.lookup(named->name);
            symbol && symbol->kind == SymbolKind::TypeParam && symbol->type)
            return symbol->type;
        if (named->name == "Self") return Type::makeTypeParam("Self");
        if (auto metadata = mContext.lookupDeclaredType(named->name);
            metadata && metadata->kind == TypeKind::Metadata) {
            const auto declaration = mContext.mQualifiedDeclarations.find(
                mContext.sourceDeclarationKey(named->name, false));
            if (declaration != mContext.mQualifiedDeclarations.end())
                mContext.recordDeclarationReference(
                    named, named->name.size(), declaration->second);
            return metadata;
        }

        TypePtr nominalType;
        const std::string typeKey = mContext.sourceDeclarationKey(named->name);
        auto nominal = mContext.mDeclaredTypes.find(typeKey);
        if (nominal != mContext.mDeclaredTypes.end()) nominalType = nominal->second;
        if (nominalType) {
            const auto declaration = mContext.mQualifiedDeclarations.find(typeKey);
            if (declaration != mContext.mQualifiedDeclarations.end())
                mContext.recordDeclarationReference(
                    named, named->name.size(), declaration->second);
            TypeVec args;
            for (auto& arg : named->typeArgs)
                args.push_back(resolveTypeAST(arg.get(), bindings));
            const_cast<NamedTypeAST*>(named)->resolvedType =
                validateTypeFormation(
                    args.empty()
                        ? nominalType
                        : instantiateNominal(nominalType, args),
                    named);
            return const_cast<NamedTypeAST*>(named)->resolvedType;
        }
        return resolveType(ast, bindings);
    }
    if (auto* ref = dynamic_cast<const RefTypeAST*>(ast))
        return validateTypeFormation(
            Type::makeReference(
                resolveTypeAST(ref->inner.get(), bindings),
                ref->isMutable),
            ref);
    if (auto* linear = dynamic_cast<const LinearTypeAST*>(ast))
        return resolveTypeAST(linear->inner.get(), bindings);
    if (auto* affine = dynamic_cast<const AffineTypeAST*>(ast))
        return resolveTypeAST(affine->inner.get(), bindings);
    if (auto* fn = dynamic_cast<const FunctionTypeAST*>(ast)) {
        TypeVec params;
        std::vector<luna::ownership::Contract> contracts;
        for (auto& param : fn->paramTypes) {
            auto type = resolveTypeAST(param.get(), bindings);
            auto usage = dynamic_cast<LinearTypeAST*>(param.get())
                ? luna::ownership::Usage::Linear
                : (dynamic_cast<AffineTypeAST*>(param.get())
                    ? luna::ownership::Usage::Affine
                    : defaultUsageForType(type));
            contracts.push_back(parameterContractFor(
                type, usage, usage != luna::ownership::Usage::Copy));
            params.push_back(std::move(type));
        }
        auto returnType = resolveTypeAST(fn->returnType.get(), bindings);
        auto returnUsage = dynamic_cast<LinearTypeAST*>(fn->returnType.get())
            ? luna::ownership::Usage::Linear
            : (dynamic_cast<AffineTypeAST*>(fn->returnType.get())
                ? luna::ownership::Usage::Affine
                : defaultUsageForType(returnType));
        return validateTypeFormation(
            Type::makeFunction(
                std::move(params), std::move(returnType),
                std::move(contracts),
                {luna::ownership::Relation::Owned, returnUsage}),
            fn);
    }
    return TyUnknown;
}

TypePtr TypeResolver::instantiateNominal(const TypePtr& type,
                                             const std::vector<TypePtr>& args) {
    if (!type || type->typeParams.empty()) return type;
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < type->typeParams.size() && i < args.size(); ++i)
        bindings[type->typeParams[i]] = args[i];
    auto instance = substituteNominalType(type, bindings);
    instance->typeArgs = args;
    instance->typeParams.clear();
    return instance;
}

TypePtr TypeResolver::declaredType(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return mContext.mConstraints.fresh();
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        if (named->name == "auto") return mContext.mConstraints.fresh();
    }
    return resolved(resolveTypeAST(ast, bindings));
}

TypePtr TypeResolver::resolved(const TypePtr& type) {
    TypePtr result = mContext.mConstraints.resolve(type);
    if (!result || result->nominalId.empty()) return result;

    // Generic nominal instances can be created while declarations are being
    // collected, before a later Drop impl is validated. Refresh only the
    // compiler-derived resource/drop facts from the owning declaration so a
    // cached Rc<T>-shaped instance cannot retain a stale Copy contract.
    for (const auto& [_, declaration] : mContext.mDeclaredTypes) {
        if (!declaration || declaration.get() == result.get() ||
            declaration->nominalId != result->nominalId)
            continue;
        if (declaration->sysmeta.resource.needsDrop) {
            result->sysmeta.resource = declaration->sysmeta.resource;
            result->sysmeta.abi.dropGlueSymbol =
                declaration->sysmeta.abi.dropGlueSymbol;
            break;
        }
    }
    return result;
}

bool TypeResolver::constrain(const TypePtr& actual, const TypePtr& expected,
                                 const std::string& context) {
    if (!actual || !expected || actual->kind == TypeKind::Unknown ||
        expected->kind == TypeKind::Unknown) return true;
    // `never` is the bottom type: a diverging expression can inhabit every
    // expected value type, while ordinary values cannot inhabit `never`.
    if (resolved(actual)->kind == TypeKind::Never) return true;
    const TypePtr expectedType = resolved(expected);
    if (expectedType->kind != TypeKind::InferenceVar &&
        !isNumericType(expectedType))
        mContext.mConstraints.defaultNumeric(actual);
    std::string reason;
    if (!mContext.mConstraints.unify(actual, expected, &reason)) {
        mContext.error("Type constraint failed in " + context + ": " + reason);
        return false;
    }
    return true;
}

void TypeResolver::requireBool(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mContext.mConstraints.requireBool(t);
        return;
    }
    if (t->kind != TypeKind::Bool)
        mContext.error(context + " must be bool, got " + t->toString());
}

void TypeResolver::requireNumeric(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mContext.mConstraints.requireNumeric(t);
        return;
    }
    if (!isNumericType(t))
        mContext.error(context + " must be numeric, got " + t->toString());
}

void TypeResolver::requireInteger(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    // Integer literals and currently-unbound inferred variables default to i32
    // later in inference. Keep them accepted here while rejecting floats and
    // non-numeric values immediately.
    if (t->kind == TypeKind::InferenceVar) {
        mContext.mConstraints.requireNumeric(t);
        return;
    }
    if (!isIntegerType(t))
        mContext.error(context + " must be an integer, got " + t->toString());
}

void TypeResolver::checkUnresolved(const TypePtr& type, const std::string& context) {
    if (type && mContext.mConstraints.hasUnresolved(type))
        mContext.error("Could not infer " + context);
}

std::unique_ptr<TypeAST> TypeResolver::typeToAST(const TypePtr& type) {
    auto t = resolved(type);
    if (!t || t->kind == TypeKind::Unknown || t->kind == TypeKind::InferenceVar)
        return std::make_unique<NamedTypeAST>("i32");
    if (t->kind == TypeKind::Record) {
        auto record = std::make_unique<RecordTypeAST>();
        record->resolvedType = t;
        for (const auto& field : t->fields) {
            RecordTypeAST::Field astField;
            astField.name = field.name;
            astField.type = typeToAST(field.type);
            record->fields.push_back(std::move(astField));
        }
        return record;
    }
    if (t->kind == TypeKind::Reference)
        return std::make_unique<RefTypeAST>(typeToAST(t->inner), t->isMutable);
    if (t->kind == TypeKind::RawPointer) {
        auto raw = std::make_unique<NamedTypeAST>("raw");
        raw->typeArgs.push_back(typeToAST(t->inner));
        return raw;
    }
    if (t->kind == TypeKind::Result && t->typeArgs.size() == 2) {
        auto result = std::make_unique<NamedTypeAST>("Result");
        result->typeArgs.push_back(typeToAST(t->typeArgs[0]));
        result->typeArgs.push_back(typeToAST(t->typeArgs[1]));
        return result;
    }
    if (t->kind == TypeKind::DeviceBuffer) {
        auto buffer = std::make_unique<NamedTypeAST>("device_buffer");
        buffer->typeArgs.push_back(typeToAST(t->inner));
        return buffer;
    }
    if (t->kind == TypeKind::Array) {
        auto array = std::make_unique<NamedTypeAST>("array");
        array->typeArgs.push_back(typeToAST(t->inner));
        array->arrayLength = t->arrayLength;
        return array;
    }
    if (t->kind == TypeKind::Slice) {
        auto slice = std::make_unique<NamedTypeAST>("slice");
        slice->typeArgs.push_back(typeToAST(t->inner));
        return slice;
    }
    if (t->kind == TypeKind::Event)
        return std::make_unique<NamedTypeAST>("event");
    if (t->kind == TypeKind::Function) {
        auto fn = std::make_unique<FunctionTypeAST>();
        for (auto& p : t->paramTypes) fn->paramTypes.push_back(typeToAST(p));
        fn->returnType = typeToAST(t->returnType);
        return fn;
    }
    if (t->kind == TypeKind::Closure) {
        auto named = std::make_unique<NamedTypeAST>(t->toString());
        named->resolvedType = t;
        return named;
    }
    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) {
        auto named = std::make_unique<NamedTypeAST>(t->name);
        named->resolvedType = t;
        for (auto& arg : t->typeArgs)
            named->typeArgs.push_back(typeToAST(arg));
        return named;
    }
    if (t->kind == TypeKind::TypeParam)
        return std::make_unique<NamedTypeAST>(t->name);
    return std::make_unique<NamedTypeAST>(t->toString());
}

void TypeResolver::materializeInferredTypes(Program* program) {
    std::function<void(Expr*)> visitExpr;
    std::function<void(BlockStmt*)> visitBlock;
    std::function<void(Stmt*)> visitStmt;
    const auto needsConcreteAnnotation = [](const std::unique_ptr<TypeAST>& type) {
        if (!type) return true;
        const auto* named = dynamic_cast<const NamedTypeAST*>(type.get());
        return named && named->name == "auto";
    };
    const auto validateIntegerLiteral = [this](
        IntLiteralExpr* literal, bool negative) {
        literal->inferredType = literal->inferredType
            ? resolved(literal->inferredType) : TyI32;
        if (!integerLiteralFits(
                literal->magnitude, literal->inferredType, negative)) {
            mContext.error(
                "integer literal '" +
                    std::string(negative ? "-" : "") +
                    std::to_string(literal->magnitude) +
                    "' is outside the range of " +
                    literal->inferredType->toString(),
                literal->line, literal->col);
        }
    };

    visitExpr = [&](Expr* expr) {
        if (!expr) return;
        if (auto* literal = dynamic_cast<IntLiteralExpr*>(expr)) {
            validateIntegerLiteral(literal, false);
            return;
        }
        if (auto* l = dynamic_cast<LambdaExpr*>(expr)) {
            for (auto& p : l->params) {
                checkUnresolved(p.inferredType, "lambda parameter '" + p.name + "'");
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
            }
            checkUnresolved(l->closureType, "lambda type");
            l->closureType = resolved(l->closureType);
            if (needsConcreteAnnotation(l->returnType) && l->closureType &&
                (l->closureType->kind == TypeKind::Function ||
                 l->closureType->kind == TypeKind::Closure))
                l->returnType = typeToAST(l->closureType->returnType);
            visitBlock(l->body.get());
            return;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(expr)) { visitExpr(b->lhs.get()); visitExpr(b->rhs.get()); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(expr)) {
            if (u->op == TokenKind::Minus) {
                if (auto* literal =
                        dynamic_cast<IntLiteralExpr*>(u->operand.get())) {
                    validateIntegerLiteral(literal, true);
                    return;
                }
            }
            visitExpr(u->operand.get());
            return;
        }
        if (auto* c = dynamic_cast<CallExpr*>(expr)) {
            if (c->intrinsicType)
                c->intrinsicType = resolved(c->intrinsicType);
            if (c->resultType)
                c->resultType = resolved(c->resultType);
            if (c->iteratorInputType)
                c->iteratorInputType = resolved(c->iteratorInputType);
            if (c->iteratorOutputType)
                c->iteratorOutputType = resolved(c->iteratorOutputType);
            for (auto& type : c->typeArgs) type = resolved(type);
            visitExpr(c->callee.get());
            for (auto& a : c->args) visitExpr(a.get());
            return;
        }
        if (auto* s = dynamic_cast<SelectExpr*>(expr)) {
            if (s->selectedType)
                s->selectedType = resolved(s->selectedType);
            for (auto& a : s->selectorArgs) visitExpr(a.get());
            return;
        }
        if (auto* l = dynamic_cast<LaunchExpr*>(expr)) {
            visitExpr(l->threads.get()); for (auto& a : l->args) visitExpr(a.get()); return;
        }
        if (auto* v = dynamic_cast<VariantConstructExpr*>(expr)) {
            v->constructedType = resolved(v->constructedType);
            for (auto& a : v->args) visitExpr(a.get());
            return;
        }
        if (auto* a = dynamic_cast<AssignExpr*>(expr)) { visitExpr(a->lhs.get()); visitExpr(a->rhs.get()); return; }
        if (auto* f = dynamic_cast<FieldAccessExpr*>(expr)) { visitExpr(f->object.get()); return; }
        if (auto* i = dynamic_cast<IndexExpr*>(expr)) { visitExpr(i->object.get()); visitExpr(i->index.get()); return; }
        if (auto* a = dynamic_cast<ArrayLiteralExpr*>(expr)) {
            a->elementType = resolved(a->elementType);
            for (auto& element : a->elements) visitExpr(element.get());
            return;
        }
        if (auto* r = dynamic_cast<RecordLiteralExpr*>(expr)) {
            r->recordType = resolved(r->recordType);
            for (auto& field : r->fields) visitExpr(field.value.get());
            return;
        }
        if (auto* h = dynamic_cast<HeapAllocExpr*>(expr)) { visitExpr(h->initializer.get()); return; }
        if (auto* m = dynamic_cast<MoveExpr*>(expr)) { visitExpr(m->operand.get()); return; }
        if (auto* b = dynamic_cast<BorrowExpr*>(expr)) { visitExpr(b->operand.get()); return; }
        if (auto* d = dynamic_cast<DerefExpr*>(expr)) {
            d->resultType = resolved(d->resultType);
            visitExpr(d->operand.get());
            return;
        }
        if (auto* a = dynamic_cast<AddrOfExpr*>(expr)) { visitExpr(a->operand.get()); return; }
        if (auto* t = dynamic_cast<TryExpr*>(expr)) {
            visitExpr(t->operand.get());
            t->resultType = resolved(t->resultType);
            t->propagatedResultType = resolved(t->propagatedResultType);
            t->valueType = resolved(t->valueType);
            t->errorType = resolved(t->errorType);
            t->propagatedErrorType = resolved(t->propagatedErrorType);
            return;
        }
        if (auto* i = dynamic_cast<IfExpr*>(expr)) { visitExpr(i->cond.get()); visitExpr(i->thenExpr.get()); visitExpr(i->elseExpr.get()); return; }
        if (auto* b = dynamic_cast<BlockExpr*>(expr)) { visitBlock(b->block.get()); return; }
    };
    visitStmt = [&](Stmt* stmt) {
        if (!stmt) return;
        if (auto* b = dynamic_cast<BlockStmt*>(stmt)) { visitBlock(b); return; }
        if (auto* l = dynamic_cast<LetStmt*>(stmt)) {
            l->inferredType = resolved(l->inferredType);
            visitExpr(l->initializer.get());
            return;
        }
        if (auto* r = dynamic_cast<ReturnStmt*>(stmt)) { visitExpr(r->value.get()); return; }
        if (auto* a = dynamic_cast<AwaitStmt*>(stmt)) { visitExpr(a->event.get()); return; }
        if (auto* e = dynamic_cast<ExprStmt*>(stmt)) { visitExpr(e->expr.get()); return; }
        if (auto* i = dynamic_cast<IfStmt*>(stmt)) { visitExpr(i->cond.get()); visitBlock(i->thenBlock.get()); visitStmt(i->elseBranch.get()); return; }
        if (auto* m = dynamic_cast<MatchStmt*>(stmt)) {
            visitExpr(m->scrutinee.get());
            m->matchedType = resolved(m->matchedType);
            for (auto& arm : m->arms) {
                for (auto& type : arm.bindingTypes)
                    type = resolved(type);
                visitBlock(arm.body.get());
            }
            return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(stmt)) { visitExpr(w->cond.get()); visitBlock(w->body.get()); return; }
        if (auto* f = dynamic_cast<ForStmt*>(stmt)) {
            visitExpr(f->iterable.get());
            f->elementType = resolved(f->elementType);
            if (f->protocolIteratorType)
                f->protocolIteratorType =
                    resolved(f->protocolIteratorType);
            if (f->protocolOptionType)
                f->protocolOptionType =
                    resolved(f->protocolOptionType);
            if (f->protocolInputType)
                f->protocolInputType =
                    resolved(f->protocolInputType);
            if (f->recipeSourceType)
                f->recipeSourceType =
                    resolved(f->recipeSourceType);
            visitBlock(f->body.get());
            return;
        }
        if (auto* f = dynamic_cast<FreeStmt*>(stmt)) { visitExpr(f->operand.get()); return; }
        if (auto* s = dynamic_cast<SlotInvokeStmt*>(stmt)) {
            s->structuralType = resolved(s->structuralType);
            for (auto& type : s->interfaceParams)
                type.inferredType = resolved(type.inferredType);
            for (auto& argument : s->args) visitExpr(argument.get());
            visitBlock(s->continuation.get());
            return;
        }
        if (auto* a = dynamic_cast<ApplyStmt*>(stmt)) {
            visitBlock(a->body.get());
            return;
        }
    };
    visitBlock = [&](BlockStmt* block) {
        if (!block) return;
        for (auto& stmt : block->stmts) visitStmt(stmt.get());
    };

    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            for (auto& p : f->params) {
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
            }
            f->inferredReturnType = resolved(f->inferredReturnType);
            if (needsConcreteAnnotation(f->returnType))
                f->returnType = typeToAST(f->inferredReturnType);
            visitBlock(f->body.get());
        } else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& f : i->methods) {
                for (auto& p : f->params) {
                    p.inferredType = resolved(p.inferredType);
                    if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
                }
                f->inferredReturnType = resolved(f->inferredReturnType);
                if (needsConcreteAnnotation(f->returnType))
                    f->returnType = typeToAST(f->inferredReturnType);
                visitBlock(f->body.get());
            }
        } else if (auto* f = dynamic_cast<FragmentDecl*>(decl.get())) {
            f->structuralType = resolved(f->structuralType);
            for (auto& p : f->params) {
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type))
                    p.type = typeToAST(p.inferredType);
            }
            visitBlock(f->body.get());
        } else if (auto* s = dynamic_cast<SlotDecl*>(decl.get())) {
            s->structuralType = resolved(s->structuralType);
            for (auto& p : s->params) {
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type))
                    p.type = typeToAST(p.inferredType);
            }
        }
    }
}
