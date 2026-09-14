#include "BodyAnalyzer.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include "BodyAnalyzerInternal.h"

TypePtr BodyAnalyzer::analyzeCall(CallExpr* call) {
    auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get());
    if (auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get()))
        return analyzeMemberCall(call, member);
    if (auto* selection = dynamic_cast<SelectExpr*>(call->callee.get())) {
        auto selected = mContext.resolved(analyzeSelect(selection));
        if (selected->kind != TypeKind::Function) return TyUnknown;
        if (selected->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch for selected declaration family '" +
                  selection->targetName + "'", call->line, call->col);
            return TyUnknown;
        }
        for (size_t index = 0; index < call->args.size(); ++index)
            mContext.constrain(analyzeExpr(call->args[index].get()), selected->paramTypes[index],
                      "selected call argument " + std::to_string(index + 1));
        call->resolvedSymbolName = selection->resolvedSymbolName;
        call->resultType = selected->returnType;
        return selected->returnType;
    }
    if (id) {
        if (auto result = analyzeIntrinsicCall(call, id))
            return result;
    }
    if (id && (id->name == "type_of" || id->name == "type_kind" ||
               id->name == "type_id" || id->name == "type_shape" ||
               id->name == "type_domain" || id->name == "type_nominal" ||
               id->name == "type_size" || id->name == "type_alignment" ||
               id->name == "type_field_count" || id->name == "type_field_name" ||
               id->name == "type_field_type" || id->name == "type_variant_count" ||
               id->name == "type_variant_name" || id->name == "type_variant_field_count" ||
               id->name == "type_is_struct" || id->name == "type_is_enum" ||
               id->name == "type_is_nominal" || id->name == "type_is_structural" ||
               id->name == "type_is_meta" || id->name == "type_is_reference" ||
               id->name == "type_same" || id->name == "type_same_shape" ||
               id->name == "type_abi_compatible"))
        return mContext.analyzeReflectionCall(call, id->name);
    if (id && id->name == "slice") {
        if (call->args.size() != 3) { mContext.error("slice expects `slice(borrow array, start, end)`", call->line, call->col); return TyUnknown; }
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr source = mContext.resolved(analyzeExpr(call->args[0].get()));
        if (!borrow || source->kind != TypeKind::Reference || !source->inner || source->inner->kind != TypeKind::Array) {
            mContext.error("slice requires an explicit `borrow array` as its first argument", call->line, call->col); return TyUnknown;
        }
        mContext.requireInteger(analyzeExpr(call->args[1].get()), "slice start");
        mContext.requireInteger(analyzeExpr(call->args[2].get()), "slice end");
        call->resultType = Type::makeSlice(source->inner->inner);
        return call->resultType;
    }
    if (id && (id->name == "gpu_alloc_i32" || id->name == "gpu_load_i32" ||
               id->name == "gpu_store_i32" || id->name == "gpu_free" ||
               id->name == "gpu_copy_from_host_i32" ||
               id->name == "gpu_copy_to_host_i32")) {
        auto requireCount = [this, call, id](size_t expected) {
            if (call->args.size() != expected) {
                mContext.error("'" + id->name + "' expects " + std::to_string(expected) +
                      " argument" + (expected == 1 ? "" : "s"), call->line, call->col);
                return false;
            }
            return true;
        };
        if (id->name == "gpu_alloc_i32") {
            if (!requireCount(1)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'gpu_alloc_i32' is a host operation and cannot run inside a kernel", call->line, call->col);
            mContext.requireInteger(analyzeExpr(call->args[0].get()), "gpu_alloc_i32 element count");
            if (body_analyzer_detail::isNegativeIntegerLiteral(call->args[0].get()))
                mContext.error("'gpu_alloc_i32' requires a non-negative element count",
                               call->args[0]->line, call->args[0]->col);
            call->resultType = Type::makeDeviceBuffer(TyI32);
            return call->resultType;
        }
        if (id->name == "gpu_free") {
            if (!requireCount(1)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'gpu_free' is a host operation and cannot run inside a kernel", call->line, call->col);
            if (!dynamic_cast<MoveExpr*>(call->args[0].get()))
                mContext.error("'gpu_free' requires `move buffer` to make ownership transfer explicit",
                      call->line, call->col);
            mContext.constrain(analyzeExpr(call->args[0].get()), Type::makeDeviceBuffer(TyI32),
                      "gpu_free buffer");
            call->resultType = TyUnit;
            return call->resultType;
        }
        if (id->name == "gpu_copy_from_host_i32" || id->name == "gpu_copy_to_host_i32") {
            if (!requireCount(3)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'" + id->name + "' is a host operation and cannot run inside a kernel",
                      call->line, call->col);
            const bool isUpload = id->name == "gpu_copy_from_host_i32";
            auto* deviceBorrow = dynamic_cast<BorrowExpr*>(call->args[0 + (isUpload ? 0 : 1)].get());
            auto* hostBorrow = dynamic_cast<BorrowExpr*>(call->args[1 - (isUpload ? 0 : 1)].get());
            TypePtr deviceActual = analyzeExpr(call->args[isUpload ? 0 : 1].get());
            TypePtr hostActual = analyzeExpr(call->args[isUpload ? 1 : 0].get());
            const bool needsMutableDevice = isUpload;
            const bool needsMutableHost = !isUpload;
            if (!deviceBorrow || deviceBorrow->isMutable != needsMutableDevice)
                mContext.error("'" + id->name + "' requires " +
                      std::string(needsMutableDevice ? "`borrow mut buffer`" : "`borrow buffer`") +
                      " for its device-buffer argument", call->line, call->col);
            if (!hostBorrow || hostBorrow->isMutable != needsMutableHost)
                mContext.error("'" + id->name + "' requires " +
                      std::string(needsMutableHost ? "`borrow mut host`" : "`borrow host`") +
                      " for its host raw-pointer argument", call->line, call->col);
            mContext.constrain(deviceActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), needsMutableDevice),
                      "device-buffer argument of '" + id->name + "'");
            mContext.constrain(hostActual,
                      Type::makeReference(Type::makeRawPointer(TyI32), needsMutableHost),
                      "host raw-pointer argument of '" + id->name + "'");
            mContext.requireInteger(analyzeExpr(call->args[2].get()),
                           "element count of '" + id->name + "'");
            if (body_analyzer_detail::isNegativeIntegerLiteral(call->args[2].get()))
                mContext.error("'" + id->name + "' requires a non-negative element count",
                      call->args[2]->line, call->args[2]->col);
            call->resultType = TyUnit;
            return call->resultType;
        }
        const bool isStore = id->name == "gpu_store_i32";
        if (!requireCount(isStore ? 3 : 2)) return TyUnknown;
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr bufferActual = analyzeExpr(call->args[0].get());
        TypePtr resolvedBufferActual = mContext.resolved(bufferActual);
        const bool isDeviceReference = resolvedBufferActual->kind == TypeKind::Reference &&
            resolvedBufferActual->inner &&
            resolvedBufferActual->inner->kind == TypeKind::DeviceBuffer;
        if ((!borrow && !isDeviceReference) || (borrow && isStore && !borrow->isMutable))
            mContext.error(std::string("'") + id->name + "' requires an explicit " +
                  (isStore ? "`borrow mut buffer`" : "`borrow buffer`") +
                  " (or an already-borrowed kernel parameter)",
                  call->line, call->col);
        if (isStore) {
            mContext.constrain(bufferActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), true),
                      std::string("first argument of '") + id->name + "'");
        } else if (isDeviceReference) {
            // A mutable kernel parameter is also valid for a read-only load.
            mContext.constrain(resolvedBufferActual->inner, Type::makeDeviceBuffer(TyI32),
                      std::string("first argument of '") + id->name + "'");
        }
        mContext.requireInteger(analyzeExpr(call->args[1].get()), std::string("index argument of '") + id->name + "'");
        if (body_analyzer_detail::isNegativeIntegerLiteral(call->args[1].get()))
            mContext.error("'" + id->name + "' requires a non-negative index",
                           call->args[1]->line, call->args[1]->col);
        if (isStore) mContext.constrain(analyzeExpr(call->args[2].get()), TyI32,
                               "value argument of 'gpu_store_i32'");
        call->resultType = isStore ? TyUnit : TyI32;
        return call->resultType;
    }
    if (mContext.mInKernel && id) {
        mContext.error("kernel body may only call device built-ins (`gpu_load_i32` and `gpu_store_i32`) in the initial ABI",
              call->line, call->col);
        return TyUnknown;
    }
    SymbolInfo* sym = id ? mContext.lookupSymbol(id->name) : nullptr;
    auto constrainArgument = [this](Expr* expr, const TypePtr& expected,
                                     const std::string& context) {
        TypePtr actual = analyzeExpr(expr);
        bool isQueryPayload = false;
        if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr)) {
            if (const auto* symbol =
                    mContext.lookupSymbol(identifier->name))
                isQueryPayload =
                    symbol->isCompileTimeOptionalPayload ||
                    symbol->isCompileTimeQueryDeclarationRef ||
                    symbol->isCompileTimeQueryDeclarationView;
        } else if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            isQueryPayload =
                call->isCompileTimeQueryDeclarationRef ||
                call->isCompileTimeQueryDeclarationView;
        }
        if (body_analyzer_detail::isCompilerOnlyValue(mContext.resolved(actual)) ||
            isQueryPayload) {
            mContext.error("compiler-only value cannot cross a function "
                  "call boundary; consume it before the call",
                  expr->line, expr->col);
            return;
        }
        // Integer-to-floating conversion is still handled by the existing
        // call boundary. Integer parameters now contextually type literals,
        // which preserves unsigned width and signedness into MoonIR.
        if (dynamic_cast<IntLiteralExpr*>(expr) &&
            isNumericType(mContext.resolved(expected)) &&
            !isIntegerType(mContext.resolved(expected)))
            return;
        if (dynamic_cast<StringLiteralExpr*>(expr) &&
            mContext.resolved(expected)->kind == TypeKind::CStr)
            return;
        mContext.constrain(actual, expected, context);
    };
    const auto analyzeDeclarationRefCall =
        [&](TypePtr reference, const std::string& declarationId,
            IdentifierExpr* identifier) -> TypePtr {
            reference = mContext.resolved(reference);
            if (!reference || reference->kind != TypeKind::DeclarationRef)
                return nullptr;
            if (!reference->inner ||
                mContext.resolved(reference->inner)->kind !=
                    TypeKind::Function) {
                mContext.error("declaration_ref target of type '" +
                      (reference->inner
                          ? reference->inner->toString()
                          : std::string{"?"}) + "' is not callable",
                      call->line, call->col);
                return TyUnknown;
            }
            const auto* selected =
                mContext.mSymbolCatalog &&
                        mContext.mSymbolCatalog->valid()
                    ? mContext.mSymbolCatalog->findDeclaration(
                          declarationId)
                    : nullptr;
            if (!selected || selected->kind !=
                    luna::selector::CatalogSymbolKind::Function) {
                bool isStaticQueryLoopBinding = false;
                if (identifier) {
                    if (const auto* binding =
                            mContext.lookupSymbol(identifier->name))
                        isStaticQueryLoopBinding =
                            binding->isCompileTimeQueryDeclarationRef;
                }
                if ((mInCompileTimeInactiveBranch ||
                     isStaticQueryLoopBinding) && declarationId.empty()) {
                    TypePtr callable =
                        mContext.resolved(reference->inner);
                    if (callable->paramTypes.size() != call->args.size()) {
                        mContext.error("Argument count mismatch in inactive "
                              "declaration_ref call",
                              call->line, call->col);
                        return TyUnknown;
                    }
                    for (size_t index = 0;
                         index < call->args.size(); ++index)
                        constrainArgument(
                            call->args[index].get(),
                            callable->paramTypes[index],
                            "inactive declaration_ref call argument");
                    call->returnsLinear =
                        callable->returnContract.usage ==
                            luna::ownership::Usage::Linear;
                    call->returnUsage =
                        callable->returnContract.usage;
                    call->resultType = callable->returnType;
                    return call->resultType;
                }
                mContext.error("declaration_ref call target is not present in "
                      "the active Symbol Catalog", call->line, call->col);
                return TyUnknown;
            }
            TypePtr callable = mContext.resolved(reference->inner);
            if (callable->paramTypes.size() != call->args.size()) {
                mContext.error("Argument count mismatch in declaration_ref call",
                      call->line, call->col);
                return TyUnknown;
            }
            for (size_t index = 0; index < call->args.size(); ++index)
                constrainArgument(call->args[index].get(),
                    callable->paramTypes[index],
                    "declaration_ref call argument");
            call->resolvedSymbolName = selected->symbolName;
            call->resolvedSymbolId = selected->symbolId;
            call->resolvedContractId = selected->contractId;
            if (identifier)
                identifier->resolvedSymbolName = selected->symbolName;
            call->returnsLinear =
                callable->returnContract.usage ==
                    luna::ownership::Usage::Linear;
            call->returnUsage = callable->returnContract.usage;
            call->resultType = callable->returnType;
            return call->resultType;
        };

    if (id) {
        const std::string declarationKey = mContext.sourceDeclarationKey(id->name);
        auto family = mContext.mFunctionFamilies.find(declarationKey);
        if (family != mContext.mFunctionFamilies.end() && family->second.size() > 1) {
            mContext.error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", call->line, call->col);
            return TyUnknown;
        }
        if (sym && sym->kind == SymbolKind::Function &&
            family != mContext.mFunctionFamilies.end() && family->second.size() == 1) {
            auto* declaration = family->second.front();
            call->resolvedSymbolName = declaration->generatedSymbolName.empty()
                ? declaration->name : declaration->generatedSymbolName;
            id->resolvedSymbolName = call->resolvedSymbolName;
            mContext.recordDeclarationReference(id, id->name.size(), declaration);
        }
    }

    // Direct calls and closure calls both become constraints. This is the
    // important consumer side of inference: an untyped declaration can be
    // solved by the arguments at its call sites.
    if (!sym) {
        TypePtr calleeType = analyzeExpr(call->callee.get());
        if (mContext.resolved(calleeType)->kind ==
                TypeKind::DeclarationRef) {
            auto* terminal =
                dynamic_cast<CallExpr*>(call->callee.get());
            return analyzeDeclarationRefCall(
                calleeType,
                terminal ? terminal->compileTimeDeclarationId
                         : std::string{},
                nullptr);
        }
        if (calleeType->kind != TypeKind::Function &&
            calleeType->kind != TypeKind::Closure) {
            mContext.error("Expression is not callable");
            return TyUnknown;
        }
        if (calleeType->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch in indirect call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), calleeType->paramTypes[i], "call argument");
        call->resultType = calleeType->returnType;
        return call->resultType;
    }

    if (sym->kind == SymbolKind::Variable && sym->type &&
        mContext.resolved(sym->type)->kind ==
            TypeKind::DeclarationRef) {
        TypePtr result = analyzeDeclarationRefCall(
            sym->type, sym->compileTimeDeclarationId, id);
        if (!sym->compileTimeDeclarationSymbolId.empty())
            call->resolvedSymbolId =
                sym->compileTimeDeclarationSymbolId;
        if (!sym->compileTimeDeclarationContractId.empty())
            call->resolvedContractId =
                sym->compileTimeDeclarationContractId;
        return result;
    }

    if (sym->kind == SymbolKind::Variable && sym->type &&
        (mContext.resolved(sym->type)->kind == TypeKind::Function ||
         mContext.resolved(sym->type)->kind == TypeKind::Closure)) {
        auto fn = mContext.resolved(sym->type);
        if (fn->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch in closure call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), fn->paramTypes[i], "closure call argument");
        call->resultType = fn->returnType;
        return call->resultType;
    }
    if (sym->kind != SymbolKind::Function) {
        mContext.error("'" + id->name + "' is not callable");
        return TyUnknown;
    }
    if (call->args.size() != sym->paramTypes.size()) {
        mContext.error("Argument count mismatch for '" + id->name + "'");
        return TyUnknown;
    }

    // Generic functions retain their existing monomorphization path. Their
    // type parameters are explicit variables, whereas omitted ordinary
    // signatures use the inference variables in paramTypes/returnType below.
    if (!sym->typeParams.empty() && sym->genericDecl) {
        TypeVec actualTypes;
        for (auto& argument : call->args) {
            TypePtr actual = analyzeExpr(argument.get());
            // Monomorphization needs a concrete type immediately. Apply the
            // language's i32 default only when this is an unresolved numeric
            // argument; non-numeric inference remains diagnosable.
            mContext.mConstraints.defaultNumeric(actual);
            actualTypes.push_back(mContext.resolved(actual));
        }
        TypeVec concreteTypes;
        if (!call->typeArgASTs.empty()) {
            if (call->typeArgASTs.size() != sym->typeParams.size()) {
                mContext.error("generic function '" + id->name + "' expects " +
                      std::to_string(sym->typeParams.size()) +
                      " type argument(s)", call->line, call->col);
                return TyUnknown;
            }
            for (auto& typeArgument : call->typeArgASTs)
                concreteTypes.push_back(mContext.resolved(
                    mContext.resolveTypeAST(typeArgument.get(), {})));
        } else {
            std::unordered_map<std::string, TypePtr> inferred;
            std::function<void(const TypePtr&, const TypePtr&)> infer =
                [&](const TypePtr& patternValue,
                    const TypePtr& actualValue) {
                const TypePtr pattern = mContext.resolved(patternValue);
                const TypePtr actual = mContext.resolved(actualValue);
                if (!pattern || !actual) return;
                if (pattern->kind == TypeKind::TypeParam) {
                    if (std::find(sym->typeParams.begin(),
                                  sym->typeParams.end(),
                                  pattern->name) ==
                        sym->typeParams.end())
                        return;
                    auto existing = inferred.find(pattern->name);
                    if (existing == inferred.end())
                        inferred[pattern->name] = actual;
                    else if (!luna::types::sameType(
                                 existing->second, actual))
                        mContext.error("conflicting inference for generic type '" +
                              pattern->name + "'", call->line, call->col);
                    return;
                }
                if (pattern->kind != actual->kind) return;
                if (!pattern->typeArgs.empty() &&
                    pattern->typeArgs.size() ==
                        actual->typeArgs.size()) {
                    for (size_t index = 0;
                         index < pattern->typeArgs.size(); ++index)
                        infer(pattern->typeArgs[index],
                              actual->typeArgs[index]);
                }
                if (pattern->inner && actual->inner)
                    infer(pattern->inner, actual->inner);
                if (pattern->kind == TypeKind::Function &&
                    pattern->paramTypes.size() ==
                        actual->paramTypes.size()) {
                    for (size_t index = 0;
                         index < pattern->paramTypes.size(); ++index)
                        infer(pattern->paramTypes[index],
                              actual->paramTypes[index]);
                    infer(pattern->returnType, actual->returnType);
                }
            };
            for (size_t index = 0;
                 index < sym->paramTypes.size() &&
                 index < actualTypes.size(); ++index)
                infer(sym->paramTypes[index], actualTypes[index]);
            for (const auto& parameter : sym->typeParams) {
                auto found = inferred.find(parameter);
                if (found == inferred.end()) {
                    mContext.error("could not infer generic type argument '" +
                          parameter + "' for '" + id->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                concreteTypes.push_back(found->second);
            }
        }
        call->typeArgs = concreteTypes;

        std::unordered_map<std::string, TypePtr> constraintBindings;
        for (size_t index = 0;
             index < sym->genericDecl->typeParams.size() &&
             index < concreteTypes.size(); ++index)
            constraintBindings[sym->genericDecl->typeParams[index]] =
                mContext.resolved(concreteTypes[index]);
        for (size_t index = 0;
             index < sym->paramTypes.size() &&
             index < call->args.size(); ++index) {
            constrainArgument(
                call->args[index].get(),
                substituteNominalType(
                    mContext.resolved(sym->paramTypes[index]),
                    constraintBindings),
                "argument " + std::to_string(index + 1) +
                    " of generic call");
        }
        for (auto& clause : sym->genericDecl->whereClauses) {
            if (clause.kind == WhereClause::Kind::TraitBound) {
                const std::string& tpName = clause.typeParam;
                const std::string& traitId = clause.trait.resolvedTraitId;
                auto concrete = constraintBindings.find(tpName);
                if (concrete != constraintBindings.end() &&
                    !mContext.satisfiesTrait(traitId, concrete->second))
                    mContext.error("Type '" + mContext.typeIdentity(concrete->second) +
                          "' does not satisfy trait '" +
                          displayTraitRef(clause.trait) + "'");
                continue;
            }

            if (clause.kind == WhereClause::Kind::ConstraintExpression) {
                std::vector<std::string> activeConstraints;
                auto value = mContext.evaluateConstraintExpr(
                    clause.constraintExpression.get(),
                    constraintBindings, activeConstraints);
                if (!value) {
                    mContext.error(
                        "inline where predicate is not compile-time evaluable",
                        call->line, call->col);
                } else if (auto* satisfied = std::get_if<bool>(&*value)) {
                    if (!*satisfied)
                        mContext.error(
                            "inline where predicate is not satisfied",
                            call->line, call->col);
                } else {
                    mContext.error(
                        "inline where predicate must evaluate to bool",
                        call->line, call->col);
                }
                continue;
            }

            TypeVec conceptArguments;
            for (auto& argument : clause.constraintTypeArgs)
                conceptArguments.push_back(mContext.resolved(
                    mContext.resolveTypeAST(argument.get(), constraintBindings)));
            std::vector<std::string> activeConstraints;
            auto satisfied = mContext.evaluateConstraint(
                clause.constraintName, conceptArguments, activeConstraints);
            if (!satisfied) {
                mContext.error("constraint '" + clause.constraintName +
                      "' is not compile-time evaluable");
            } else if (!*satisfied) {
                std::string types;
                for (size_t index = 0; index < conceptArguments.size(); ++index) {
                    if (index) types += ", ";
                    types += conceptArguments[index]->toString();
                }
                mContext.error("constraint '" + clause.constraintName +
                      "<" + types + ">' is not satisfied");
            }
        }
        auto* specialized = mContext.monomorphize(sym->genericDecl, concreteTypes);
        if (specialized && mContext.mProgram) {
            const bool newlyCreated = !mContext.mGeneratedInstances.empty() &&
                                      mContext.mGeneratedInstances.back().get() == specialized;
            if (newlyCreated) {
                mContext.mProgram->declarations.push_back(std::move(mContext.mGeneratedInstances.back()));
                const std::string savedPackage = mContext.mCurrentPackageId;
                const std::string savedModule = mContext.mCurrentModulePath;
                mContext.setDeclarationContext(specialized);
                mContext.declareFunction(specialized);
                analyzeFunction(specialized);
                mContext.mCurrentPackageId = savedPackage;
                mContext.mCurrentModulePath = savedModule;
            }
            call->resolvedSymbolName = specialized->generatedSymbolName.empty()
                ? specialized->name : specialized->generatedSymbolName;
            call->returnsLinear = specialized->returnsLinear;
            call->returnUsage = specialized->returnUsage;
            call->resultType = specialized->inferredReturnType
                ? specialized->inferredReturnType : TyUnit;
            return call->resultType;
        }
    }

    // Built-in print is polymorphic but has no user AST to monomorphize.
    if (!sym->typeParams.empty() && !sym->genericDecl) {
        for (auto& arg : call->args) {
            const TypePtr argumentType = mContext.resolved(analyzeExpr(arg.get()));
            if (id && id->name == "print" &&
                argumentType->kind != TypeKind::InferenceVar &&
                argumentType->kind != TypeKind::I32 &&
                argumentType->kind != TypeKind::U32 &&
                argumentType->kind != TypeKind::String &&
                argumentType->kind != TypeKind::CStr) {
                mContext.error("temporary print supports only i32, u32, string, or cstr; "
                      "got " + argumentType->toString(),
                      call->line, call->col);
            }
        }
        return sym->returnType ? sym->returnType : TyUnit;
    }

    for (size_t i = 0; i < call->args.size(); ++i)
        constrainArgument(call->args[i].get(), sym->paramTypes[i],
                          "argument " + std::to_string(i + 1) + " of '" + id->name + "'");
    if (id) {
        const std::string constexprName = call->resolvedSymbolName.empty()
            ? id->name : call->resolvedSymbolName;
        auto constexprIt = mContext.mConstexprFunctions.find(constexprName);
        if (constexprIt != mContext.mConstexprFunctions.end()) {
            if (auto value = mContext.evaluateConstExpr(call)) call->compileTimeValue = std::move(*value);
        }
    }
    call->returnsLinear = sym->returnsLinear;
    call->returnUsage = sym->returnUsage;
    call->resultType = sym->returnType ? sym->returnType : TyUnit;
    return call->resultType;
}
