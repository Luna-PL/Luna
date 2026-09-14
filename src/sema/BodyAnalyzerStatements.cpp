#include "BodyAnalyzer.h"

#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"
#include "SemanticAnalysisSupport.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include "BodyAnalyzerInternal.h"

TypePtr BodyAnalyzer::analyzeLetStmt(LetStmt* ls, TypePtr expectedReturn) {
    (void)expectedReturn;
    TypePtr rhsType = analyzeExpr(ls->initializer.get());

    // Check if rhs is a HeapAllocExpr — mark as heap allocated
    bool isHeap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()) != nullptr;

    TypePtr declaredType;
    if (ls->typeAnnotation) {
        std::unordered_map<std::string, TypePtr> bindings;
        declaredType = this->mContext.declaredType(ls->typeAnnotation.get(), bindings);
        if (!(dynamic_cast<StringLiteralExpr*>(ls->initializer.get()) &&
              mContext.resolved(declaredType)->kind == TypeKind::CStr))
            mContext.constrain(rhsType, declaredType, "let binding '" + ls->name + "'");
    } else {
        declaredType = rhsType; // auto inference
    }

    SymbolInfo info;
    info.kind = SymbolKind::Variable;
    info.type = declaredType;
    info.isConst = ls->isConst;
    info.isHeapAllocated = isHeap || typeRequiresCleanup(declaredType);
    if (auto* reflected = dynamic_cast<CallExpr*>(ls->initializer.get())) {
        info.compileTimeDeclarationId = reflected->compileTimeDeclarationId;
        info.compileTimeDeclarationSymbolId = reflected->resolvedSymbolId;
        info.compileTimeDeclarationContractId = reflected->resolvedContractId;
        if (reflected->isCompileTimeSymbolSet) {
            info.isCompileTimeSymbolSet = true;
            info.compileTimeSymbolSetDeclarationIds = reflected->compileTimeSymbolSetDeclarationIds;
        }
        if (reflected->isCompileTimeQueryDeclarationView) {
            info.isCompileTimeQueryDeclarationView = true;
            info.compileTimeDeclarationViewDeclarationIds =
                reflected->compileTimeDeclarationViewDeclarationIds;
        }
        info.isCompileTimeQueryDeclarationRef = reflected->isCompileTimeQueryDeclarationRef;
        if (reflected->isCompileTimeOptionalDeclarationRef) {
            info.isCompileTimeOptionalDeclarationRef = true;
            info.compileTimeOptionalHasValue = reflected->compileTimeOptionalHasValue;
        }
    } else if (auto* identifier = dynamic_cast<IdentifierExpr*>(ls->initializer.get())) {
        if (const auto* source = mContext.lookupSymbol(identifier->name)) {
            info.compileTimeDeclarationId = source->compileTimeDeclarationId;
            info.compileTimeDeclarationSymbolId = source->compileTimeDeclarationSymbolId;
            info.compileTimeDeclarationContractId = source->compileTimeDeclarationContractId;
            info.isCompileTimeSymbolSet = source->isCompileTimeSymbolSet;
            info.compileTimeSymbolSetDeclarationIds = source->compileTimeSymbolSetDeclarationIds;
            info.isCompileTimeQueryDeclarationView = source->isCompileTimeQueryDeclarationView;
            info.compileTimeDeclarationViewDeclarationIds =
                source->compileTimeDeclarationViewDeclarationIds;
            info.isCompileTimeQueryDeclarationRef = source->isCompileTimeQueryDeclarationRef;
            info.isCompileTimeOptionalDeclarationRef = source->isCompileTimeOptionalDeclarationRef;
            info.compileTimeOptionalHasValue = source->compileTimeOptionalHasValue;
            info.isCompileTimeOptionalPayload = source->isCompileTimeOptionalPayload;
        }
    }
    auto finalType = mContext.resolved(declaredType);
    ls->inferredType = finalType;
    ls->materializesIteratorRecipe = false;
    ls->materializedIteratorOwnsSource = false;
    ls->materializedIteratorSourceType.reset();
    if (finalType->kind == TypeKind::Iterator) {
        CallExpr* base = nullptr;
        std::function<void(Expr*)> findBase = [&](Expr* expression) {
            auto* call = dynamic_cast<CallExpr*>(expression);
            if (!call) return;
            if (call->iteratorOp == IteratorOp::Range || call->iteratorOp == IteratorOp::Iter ||
                call->iteratorOp == IteratorOp::IterMut ||
                call->iteratorOp == IteratorOp::IntoIter) {
                base = call;
                return;
            }
            auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
            if (member) findBase(member->object.get());
        };
        findBase(ls->initializer.get());
        bool supported = base != nullptr;
        if (base && base->iteratorOp != IteratorOp::Range) {
            auto* member = dynamic_cast<FieldAccessExpr*>(base->callee.get());
            auto* source = member ? dynamic_cast<IdentifierExpr*>(member->object.get()) : nullptr;
            if (!source) {
                mContext.error("materialized iterator recipe "
                               "requires a local array or slice "
                               "source",
                               ls->line, ls->col);
                supported = false;
            }
            TypePtr sourceType;
            if (base->resultType && !base->resultType->typeArgs.empty())
                sourceType = mContext.resolved(base->resultType->typeArgs.front());
            if (base->iteratorOp == IteratorOp::IntoIter && sourceType &&
                sourceType->kind == TypeKind::Array && sourceType->inner &&
                defaultUsageForType(sourceType->inner) != luna::ownership::Usage::Copy) {
                if (luna::ownership::mustConsume(defaultUsageForType(sourceType))) {
                    mContext.error("materialized iterator recipe cannot hide a "
                                   "linear source obligation",
                                   ls->line, ls->col);
                    supported = false;
                } else {
                    ls->materializedIteratorOwnsSource = true;
                    ls->materializedIteratorSourceType = sourceType;
                }
            }
        }
        if (!base) {
            mContext.error("an iterator binding cannot be "
                           "re-materialized from another recipe; "
                           "consume the existing binding directly",
                           ls->line, ls->col);
        }
        ls->materializesIteratorRecipe = supported;
    }
    const bool annotationLinear = dynamic_cast<LinearTypeAST*>(ls->typeAnnotation.get()) != nullptr;
    const bool annotationAffine = dynamic_cast<AffineTypeAST*>(ls->typeAnnotation.get()) != nullptr;
    const bool hasAnnotationUsage = annotationLinear || annotationAffine;
    const auto annotationUsage =
        annotationLinear ? luna::ownership::Usage::Linear : luna::ownership::Usage::Affine;
    if (ls->hasExplicitUsage && hasAnnotationUsage && ls->usage != annotationUsage) {
        mContext.error("binding '" + ls->name + "' has conflicting explicit usage contracts",
                       ls->line, ls->col);
    }
    const bool hasExplicitUsage = ls->hasExplicitUsage || hasAnnotationUsage;
    auto requestedUsage =
        ls->hasExplicitUsage
            ? ls->usage
            : (hasAnnotationUsage
                   ? annotationUsage
                   : (ls->hasInheritedUsage ? ls->inheritedUsage : defaultUsageForType(finalType)));
    // Usage blocks (linear {}, affine {}) are syntactic sugar that only
    // constrain newly declared owning bindings. Borrowed bindings
    // (references) always keep Copy cardinality regardless of the
    // surrounding block default.
    if (finalType && finalType->kind == TypeKind::Reference && !hasExplicitUsage)
        requestedUsage = luna::ownership::Usage::Copy;
    ls->usage = finalizeBindingUsage(ls->name, finalType, ls->initializer.get(), requestedUsage,
                                     hasExplicitUsage, ls->line, ls->col);
    ls->usageResolved = true;
    ls->isLinear = ls->usage == luna::ownership::Usage::Linear;
    info.usage = ls->usage;
    info.isLinear = info.usage == luna::ownership::Usage::Linear;
    mContext.mSymTable.define(ls->name, info);
    // Device resources and completion events carry ownership semantics.
    // Preserve their inferred type on the AST so the ownership and codegen
    // passes observe the same ABI even without an explicit annotation.
    if (!ls->typeAnnotation &&
        (finalType->kind == TypeKind::DeviceBuffer || finalType->kind == TypeKind::Event ||
         finalType->kind == TypeKind::Array || finalType->kind == TypeKind::Slice))
        ls->typeAnnotation = mContext.typeToAST(finalType);
    if (ls->isConst) {
        auto value = mContext.evaluateConstExpr(ls->initializer.get());
        if (!value) {
            mContext.error("const binding '" + ls->name + "' is not a compile-time expression");
        } else {
            mContext.defineConst(ls->name, *value);
        }
    }
    return TyUnit;
}

TypePtr BodyAnalyzer::analyzeForStmt(ForStmt* fs, TypePtr expectedReturn) {
    TypePtr iterable = mContext.resolved(analyzeExpr(fs->iterable.get()));
    TypePtr element = TyI32;
    fs->isCompileTimeDeclarationViewLoop = false;
    fs->compileTimeDeclarationIds.clear();
    fs->compileTimeSymbolNames.clear();
    fs->compileTimeSymbolIds.clear();
    fs->compileTimeContractIds.clear();
    fs->protocolNextSymbol.clear();
    fs->protocolIteratorType.reset();
    fs->protocolOptionType.reset();
    fs->protocolIntoSymbol.clear();
    fs->protocolInputType.reset();
    fs->protocolStateName.clear();
    fs->protocolStateNeedsCleanup = false;
    fs->recipeStateName.clear();
    fs->recipeSourceType.reset();
    const auto markMoveOnlyRecipe = [&](Expr* source, const TypePtr& sourceType) {
        if (!sourceType || sourceType->kind != TypeKind::Array ||
            defaultUsageForType(sourceType->inner) == luna::ownership::Usage::Copy)
            return;
        if (!dynamic_cast<IdentifierExpr*>(source)) {
            mContext.error("move-only consuming array iteration "
                           "currently requires a local source binding",
                           fs->line, fs->col);
            return;
        }
        fs->recipeSourceType = sourceType;
        fs->recipeStateName = "$for.recipe." + std::to_string(fs->line) + "." +
                              std::to_string(fs->col) + "." + fs->varName;
    };
    if (iterable->kind == TypeKind::DeclarationView) {
        element = Type::makeDeclarationRef(iterable->inner);
        std::vector<std::string> declarationIds;
        if (auto* call = dynamic_cast<CallExpr*>(fs->iterable.get());
            call && call->isCompileTimeQueryDeclarationView) {
            declarationIds = call->compileTimeDeclarationViewDeclarationIds;
        } else if (auto* identifier = dynamic_cast<IdentifierExpr*>(fs->iterable.get())) {
            if (const auto* symbol = mContext.lookupSymbol(identifier->name);
                symbol && symbol->isCompileTimeQueryDeclarationView)
                declarationIds = symbol->compileTimeDeclarationViewDeclarationIds;
        }
        if (!declarationIds.empty() ||
            (dynamic_cast<CallExpr*>(fs->iterable.get()) &&
             static_cast<CallExpr*>(fs->iterable.get())->isCompileTimeQueryDeclarationView)) {
            fs->isCompileTimeDeclarationViewLoop = true;
            for (const auto& declarationId : declarationIds) {
                const auto* symbol = mContext.mSymbolCatalog
                                         ? mContext.mSymbolCatalog->findDeclaration(declarationId)
                                         : nullptr;
                if (!symbol) {
                    mContext.error("declaration_view iteration references "
                                   "a declaration outside the active Symbol Catalog",
                                   fs->line, fs->col);
                    continue;
                }
                fs->compileTimeDeclarationIds.push_back(symbol->declarationId);
                fs->compileTimeSymbolNames.push_back(symbol->symbolName);
                fs->compileTimeSymbolIds.push_back(symbol->symbolId);
                fs->compileTimeContractIds.push_back(symbol->contractId);
            }
        } else if (auto* identifier = dynamic_cast<IdentifierExpr*>(fs->iterable.get())) {
            if (const auto* symbol = mContext.lookupSymbol(identifier->name);
                symbol && symbol->isCompileTimeQueryDeclarationView)
                fs->isCompileTimeDeclarationViewLoop = true;
        }
    } else if (iterable->kind == TypeKind::MetadataView)
        element = iterable->inner;
    else if (iterable->kind == TypeKind::Iterator) {
        element = iterable->inner;
        std::function<void(Expr*)> findConsumingArray = [&](Expr* expression) {
            auto* call = dynamic_cast<CallExpr*>(expression);
            if (!call) return;
            auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
            if (!member) return;
            if (call->iteratorOp == IteratorOp::IntoIter) {
                TypePtr sourceType;
                if (call->resultType && !call->resultType->typeArgs.empty())
                    sourceType = mContext.resolved(call->resultType->typeArgs.front());
                markMoveOnlyRecipe(member->object.get(), sourceType);
                return;
            }
            findConsumingArray(member->object.get());
        };
        findConsumingArray(fs->iterable.get());
    } else if (iterable->kind == TypeKind::Slice)
        element = Type::makeReference(iterable->inner);
    else if (iterable->kind == TypeKind::Array) {
        element = iterable->inner;
        markMoveOnlyRecipe(fs->iterable.get(), iterable);
    } else {
        // User-defined loops are a closed Core protocol, not structural
        // "has a next method" duck typing.  This preserves coherence and
        // leaves compiler iterator recipes free to use their fused path.
        TraitDecl* iteratorTrait = nullptr;
        auto coreIterator = mContext.mTraits.find(luna::sysmeta::IteratorTraitId);
        if (coreIterator != mContext.mTraits.end()) iteratorTrait = coreIterator->second;
        const std::string iteratorTraitId = mContext.traitIdentity(iteratorTrait);
        FunctionDecl* next = nullptr;
        FunctionDecl* into = nullptr;
        TypePtr iteratorStateType = iterable;
        TypePtr declaredIntoItem;
        if (!iteratorTraitId.empty()) {
            auto traitImpls = mContext.mImpls.find(iteratorTraitId);
            if (traitImpls != mContext.mImpls.end()) {
                auto implementation = traitImpls->second.find(mContext.typeIdentity(iterable));
                if (implementation != traitImpls->second.end()) {
                    auto method = implementation->second.find("next");
                    if (method != implementation->second.end()) next = method->second;
                }
            }
        }

        if (!next) {
            TraitDecl* intoIteratorTrait = nullptr;
            auto coreIntoIterator = mContext.mTraits.find(luna::sysmeta::IntoIteratorTraitId);
            if (coreIntoIterator != mContext.mTraits.end())
                intoIteratorTrait = coreIntoIterator->second;
            const std::string intoTraitId = mContext.traitIdentity(intoIteratorTrait);
            if (!intoTraitId.empty()) {
                auto traitImpls = mContext.mImpls.find(intoTraitId);
                if (traitImpls != mContext.mImpls.end()) {
                    auto implementation = traitImpls->second.find(mContext.typeIdentity(iterable));
                    if (implementation != traitImpls->second.end()) {
                        auto method = implementation->second.find("into_iter");
                        if (method != implementation->second.end()) into = method->second;
                    }
                }
            }
            if (into) {
                if (into->params.size() != 1 ||
                    !luna::types::sameType(mContext.resolved(into->params.front().inferredType),
                                           iterable) ||
                    into->params.front().relation != luna::ownership::Relation::Owned) {
                    mContext.error("Core IntoIterator::into_iter must "
                                   "take ownership of exactly one '" +
                                       iterable->toString() + "' value",
                                   into->line, into->col);
                }
                iteratorStateType = mContext.resolved(into->inferredReturnType);

                // Recover the declared Item/Iter association from the
                // exact coherent impl.  Method return type alone carries
                // Iter but not the associated Item witness.
                for (const auto& declaration : mContext.mProgram->declarations) {
                    auto* implementation = dynamic_cast<ImplDecl*>(declaration.get());
                    if (!implementation || implementation->trait.resolvedTraitId != intoTraitId ||
                        implementation->resolvedTargetTypeId != mContext.typeIdentity(iterable))
                        continue;
                    if (implementation->trait.resolvedTypeArgs.size() == 2) {
                        declaredIntoItem =
                            mContext.resolved(implementation->trait.resolvedTypeArgs[0]);
                        TypePtr declaredIterator =
                            mContext.resolved(implementation->trait.resolvedTypeArgs[1]);
                        if (!luna::types::sameType(declaredIterator, iteratorStateType))
                            mContext.error("Core IntoIterator::into_iter "
                                           "return type disagrees with "
                                           "its Iter argument",
                                           into->line, into->col);
                    }
                    break;
                }

                if (!iteratorTraitId.empty()) {
                    auto traitImpls = mContext.mImpls.find(iteratorTraitId);
                    if (traitImpls != mContext.mImpls.end()) {
                        auto implementation =
                            traitImpls->second.find(mContext.typeIdentity(iteratorStateType));
                        if (implementation != traitImpls->second.end()) {
                            auto method = implementation->second.find("next");
                            if (method != implementation->second.end()) next = method->second;
                        }
                    }
                }
                if (!next)
                    mContext.error("Core IntoIterator for type '" + iterable->toString() +
                                       "' returns '" + iteratorStateType->toString() +
                                       "', which does not implement "
                                       "core::iter::Iterator",
                                   fs->line, fs->col);
            }
        }

        bool validProtocol = next != nullptr;
        if (!next) {
            if (!into)
                mContext.error("for-loop type '" + iterable->toString() +
                                   "' implements neither "
                                   "core::iter::Iterator nor "
                                   "core::iter::IntoIterator",
                               fs->line, fs->col);
        } else if (!dynamic_cast<IdentifierExpr*>(fs->iterable.get())) {
            mContext.error("Core Iterator/IntoIterator for-loop source must "
                           "currently be a local binding",
                           fs->line, fs->col);
            validProtocol = false;
        }

        if (next) {
            if (next->params.size() != 1) {
                mContext.error("Core Iterator::next must have exactly one "
                               "receiver parameter",
                               next->line, next->col);
                validProtocol = false;
            } else {
                TypePtr receiver = mContext.resolved(next->params.front().inferredType);
                if (!receiver || receiver->kind != TypeKind::Reference || !receiver->isMutable ||
                    !luna::types::sameType(mContext.resolved(receiver->inner), iteratorStateType)) {
                    mContext.error("Core Iterator::next receiver must be '&mut " +
                                       iteratorStateType->toString() + "'",
                                   next->line, next->col);
                    validProtocol = false;
                }
            }

            TypePtr option = mContext.resolved(next->inferredReturnType);
            TypePtr coreOption;
            auto optionDeclaration = mContext.mDeclaredTypes.find(luna::sysmeta::OptionTypeId);
            if (optionDeclaration != mContext.mDeclaredTypes.end())
                coreOption = mContext.resolved(optionDeclaration->second);
            const bool isCoreOption = option && coreOption && option->kind == TypeKind::Enum &&
                                      !option->nominalId.empty() &&
                                      option->nominalId == coreOption->nominalId;
            size_t noneIndex = 0;
            size_t someIndex = 0;
            bool foundNone = false;
            bool foundSome = false;
            if (isCoreOption) {
                for (size_t index = 0; index < option->variants.size(); ++index) {
                    const auto& variant = option->variants[index];
                    if (variant.name == "None" && variant.fields.empty()) {
                        noneIndex = index;
                        foundNone = true;
                    } else if (variant.name == "Some" && variant.fields.size() == 1) {
                        someIndex = index;
                        element = mContext.resolved(variant.fields.front());
                        foundSome = true;
                    }
                }
            }
            if (foundSome && declaredIntoItem &&
                !luna::types::sameType(element, declaredIntoItem)) {
                mContext.error("Core IntoIterator Item type '" + declaredIntoItem->toString() +
                                   "' disagrees with Iterator item type '" + element->toString() +
                                   "'",
                               fs->line, fs->col);
                validProtocol = false;
            }
            if (!isCoreOption || !foundNone || !foundSome) {
                mContext.error("Core Iterator::next must return "
                               "core::option::Option<Item> (resolved '" +
                                   (option ? option->toString() : std::string("?")) +
                                   "' with nominal identity '" +
                                   (option ? option->nominalId : std::string{}) + "', expected '" +
                                   (coreOption ? coreOption->nominalId : std::string{}) + "')",
                               next->line, next->col);
                validProtocol = false;
            }
            if (validProtocol) {
                fs->protocolNextSymbol =
                    next->generatedSymbolName.empty() ? next->name : next->generatedSymbolName;
                fs->protocolIteratorType = iteratorStateType;
                fs->protocolOptionType = option;
                fs->protocolNoneVariant = noneIndex;
                fs->protocolSomeVariant = someIndex;
                if (into) {
                    fs->protocolIntoSymbol =
                        into->generatedSymbolName.empty() ? into->name : into->generatedSymbolName;
                    fs->protocolInputType = iterable;
                    fs->protocolStateName = "$for.iterator." + std::to_string(fs->line) + "." +
                                            std::to_string(fs->col) + "." + fs->varName;
                }
            }
        }
    }
    fs->elementType = element;
    fs->bindingUsage = luna::ownership::strongerUsage(
        fs->hasInheritedUsage ? fs->inheritedUsage : luna::ownership::Usage::Copy,
        defaultUsageForType(element));
    mContext.mSymTable.enterScope();
    SymbolInfo vi;
    vi.kind = SymbolKind::Variable;
    vi.type = element;
    vi.usage = fs->bindingUsage;
    vi.isLinear = vi.usage == luna::ownership::Usage::Linear;
    vi.isCompileTimeQueryDeclarationRef = fs->isCompileTimeDeclarationViewLoop;
    mContext.mSymTable.define(fs->varName, vi);
    analyzeBlock(fs->body.get(), expectedReturn);
    mContext.mSymTable.exitScope();
    return TyUnit;
}

TypePtr BodyAnalyzer::analyzeStmt(Stmt* stmt, TypePtr expectedReturn) {
    mContext.setDiagnosticLocation(stmt);
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) return analyzeBlock(bs, expectedReturn);

    // A device kernel is deliberately a small DeviceMemory-only sublanguage.
    // The source-level continuation constructs are host control flow: lowering
    // them into SIMT code would require a per-lane continuation runtime and
    // could not preserve their resume semantics. Likewise, awaiting, freeing,
    // and host allocation are host-side synchronization/resource effects.
    // Keep this boundary in semantic analysis, before code generation can
    // accidentally inline a fragment or emit a host runtime call into HSACO.
    if (mContext.mInKernel) {
        const char* construct = nullptr;
        if (dynamic_cast<SlotDeclStmt*>(stmt))
            construct = "slot declaration";
        else if (dynamic_cast<SlotInvokeStmt*>(stmt))
            construct = "slot invocation";
        else if (dynamic_cast<ApplyStmt*>(stmt))
            construct = "apply binding";
        else if (dynamic_cast<ResumeStmt*>(stmt))
            construct = "resume()";
        else if (dynamic_cast<AbortStmt*>(stmt))
            construct = "abort()";
        else if (dynamic_cast<AwaitStmt*>(stmt))
            construct = "await";
        else if (dynamic_cast<FreeStmt*>(stmt))
            construct = "free";
        if (construct) {
            mContext.error("kernel body may not use " + std::string(construct) +
                               "; device kernels support only DeviceMemory operations and "
                               "structured scalar control flow",
                           stmt->line, stmt->col);
            return TyUnit;
        }
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        mContext.analyzeSlotDecl(slot);
        return TyUnit;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        mContext.analyzeSlotInvoke(slot, expectedReturn);
        return TyUnit;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        mContext.analyzeApply(apply, expectedReturn);
        return TyUnit;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mContext.mCurrentFragmentDecl)
            mContext.error("`resume()` may only appear inside a fragment", stmt->line, stmt->col);
        else if (mContext.mCurrentFragmentDecl &&
                 mContext.mCurrentFragmentDecl->kind == FragmentKind::Interceptor)
            mContext.error("`resume()` is not allowed in an interceptor; normal completion "
                           "forwards automatically",
                           stmt->line, stmt->col);
        return TyUnit;
    }
    if (dynamic_cast<AbortStmt*>(stmt)) {
        if (!mContext.mCurrentFragmentDecl)
            mContext.error("`abort()` may only appear inside an interceptor or context", stmt->line,
                           stmt->col);
        return TyUnit;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        TypePtr eventType = mContext.resolved(analyzeExpr(await->event.get()));
        if (eventType->kind != TypeKind::Event)
            mContext.error("`await` requires a launch event, got " + eventType->toString(),
                           await->line, await->col);
        return TyUnit;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt)) return analyzeLetStmt(ls, expectedReturn);
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        mContext.mSawReturn = true;
        if (rs->value) {
            TypePtr valueType = analyzeExpr(rs->value.get());
            bool isQueryPayload = false;
            if (auto* identifier = dynamic_cast<IdentifierExpr*>(rs->value.get())) {
                if (const auto* symbol = mContext.lookupSymbol(identifier->name))
                    isQueryPayload = symbol->isCompileTimeOptionalPayload ||
                                     symbol->isCompileTimeQueryDeclarationRef ||
                                     symbol->isCompileTimeQueryDeclarationView;
            } else if (auto* call = dynamic_cast<CallExpr*>(rs->value.get())) {
                isQueryPayload = call->isCompileTimeQueryDeclarationRef ||
                                 call->isCompileTimeQueryDeclarationView;
            }
            if (body_analyzer_detail::isCompilerOnlyValue(mContext.resolved(valueType)) ||
                isQueryPayload) {
                mContext.error("compiler-only value cannot cross a function "
                               "return boundary; consume it before returning",
                               rs->line, rs->col);
            } else if (!(dynamic_cast<StringLiteralExpr*>(rs->value.get()) &&
                         mContext.resolved(mContext.mCurrentReturnType)->kind == TypeKind::CStr)) {
                mContext.constrain(valueType, mContext.mCurrentReturnType, "return statement");
            }
            luna::ownership::Usage returningUsage = luna::ownership::Usage::Copy;
            if (auto* call = dynamic_cast<CallExpr*>(rs->value.get())) {
                returningUsage =
                    call->returnsLinear ? luna::ownership::Usage::Linear : call->returnUsage;
            } else if (auto* id = dynamic_cast<IdentifierExpr*>(rs->value.get())) {
                if (auto* symbol = mContext.mSymTable.lookup(id->name))
                    returningUsage =
                        symbol->isLinear ? luna::ownership::Usage::Linear : symbol->usage;
            }
            if (returningUsage == luna::ownership::Usage::Linear &&
                mContext.mCurrentFunctionReturnUsage != luna::ownership::Usage::Linear) {
                mContext.error(
                    "returning a linear value requires a linear function return contract", rs->line,
                    rs->col);
            } else if (returningUsage != luna::ownership::Usage::Linear &&
                       mContext.mCurrentFunctionReturnUsage == luna::ownership::Usage::Linear) {
                mContext.error(
                    "function declared with `-> linear raw<T>` must return an owning value",
                    rs->line, rs->col);
            } else if (returningUsage == luna::ownership::Usage::Affine &&
                       mContext.mCurrentFunctionReturnUsage == luna::ownership::Usage::Copy) {
                mContext.error(
                    "returning an affine value requires an affine function return contract",
                    rs->line, rs->col);
            }
        } else {
            mContext.constrain(TyUnit, mContext.mCurrentReturnType, "unit return statement");
        }
        return TyUnit;
    }
    if (auto* is = dynamic_cast<IfStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(is->cond.get());
        mContext.requireBool(condType, "if condition");
        analyzeBlock(is->thenBlock.get(), expectedReturn);
        if (is->elseBranch) analyzeStmt(is->elseBranch.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt))
        return analyzeMatchStmt(match, expectedReturn);
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(ws->cond.get());
        mContext.requireBool(condType, "while condition");
        analyzeBlock(ws->body.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt)) return analyzeForStmt(fs, expectedReturn);
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) { return analyzeExpr(es->expr.get()); }
    if (auto* fs = dynamic_cast<FreeStmt*>(stmt)) {
        analyzeExpr(fs->operand.get());
        return TyUnit;
    }
    return TyUnit;
}

TypePtr BodyAnalyzer::analyzeBlock(BlockStmt* block, TypePtr expectedReturn) {
    mContext.mSymTable.enterScope();
    mContext.enterConstScope();
    mContext.enterSlotScope();
    for (auto& stmt : block->stmts) {
        analyzeStmt(stmt.get(), expectedReturn);
    }
    mContext.mSymTable.exitScope();
    mContext.exitConstScope();
    mContext.exitSlotScope();
    return TyUnit;
}

bool BodyAnalyzer::statementAlwaysReturns(const Stmt* stmt) const {
    if (!stmt) return false;
    if (dynamic_cast<const ReturnStmt*>(stmt)) return true;
    if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        if (auto* call = dynamic_cast<const CallExpr*>(expression->expr.get())) {
            if ((call->resultType && call->resultType->kind == TypeKind::Never) ||
                (call->intrinsicType && call->intrinsicType->kind == TypeKind::Never))
                return true;
        }
    }
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) return blockAlwaysReturns(block);
    if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        return conditional->elseBranch && blockAlwaysReturns(conditional->thenBlock.get()) &&
               statementAlwaysReturns(conditional->elseBranch.get());
    }
    if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        return !match->arms.empty() &&
               std::all_of(match->arms.begin(), match->arms.end(),
                           [&](const MatchArm& arm) { return blockAlwaysReturns(arm.body.get()); });
    }
    if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt))
        return apply->body && blockAlwaysReturns(apply->body.get());
    return false;
}

bool BodyAnalyzer::blockAlwaysReturns(const BlockStmt* block) const {
    if (!block) return false;
    for (const auto& stmt : block->stmts) {
        if (statementAlwaysReturns(stmt.get())) return true;
    }
    return false;
}

// ─── Expression analysis ───────────────────────────────────────────
