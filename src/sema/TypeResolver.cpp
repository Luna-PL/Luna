#include "TypeResolver.h"

#include "SemanticAnalysisSupport.h"
#include "../parser/AST.h"
#include <functional>
#include <utility>

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

// ─── AST cloning helpers for monomorphization ────────────────────

class MonomorphizationCloner {
public:
    explicit MonomorphizationCloner(
        const std::unordered_map<std::string, TypePtr>& typeBindings)
        : bindings(typeBindings) {}

    std::unique_ptr<BlockStmt> cloneBlock(const BlockStmt* src) {
        if (!src) return nullptr;
        auto clone = located(std::make_unique<BlockStmt>(), src);
        for (const auto& statement : src->stmts) {
            auto clonedStatement = cloneStmt(statement.get());
            if (!clonedStatement) return nullptr;
            clone->stmts.push_back(std::move(clonedStatement));
        }
        return clone;
    }

    const std::string& failure() const { return failureMessage; }

private:
    const std::unordered_map<std::string, TypePtr>& bindings;
    std::string failureMessage;

    template <typename T>
    static std::unique_ptr<T> located(
        std::unique_ptr<T> clone, const ASTNode* source) {
        if (clone && source) {
            clone->sourcePath = source->sourcePath;
            clone->line = source->line;
            clone->col = source->col;
        }
        return clone;
    }

    void fail(const char* category) {
        if (failureMessage.empty())
            failureMessage =
                std::string("unsupported ") + category +
                " AST node in generic function body";
    }

    TypePtr substitute(const TypePtr& type) const {
        return type ? substituteNominalType(type, bindings) : nullptr;
    }

    CleanupObligation cloneCleanup(const CleanupObligation& source) const {
        CleanupObligation clone;
        clone.place = source.place;
        clone.action = source.action;
        clone.type = substitute(source.type);
        return clone;
    }

    std::unique_ptr<TypeAST> cloneType(const TypeAST* src) {
        if (!src) return nullptr;
        if (auto* type = dynamic_cast<const RecordTypeAST*>(src)) {
            auto clone = std::make_unique<RecordTypeAST>();
            clone->resolvedType = substitute(type->resolvedType);
            for (const auto& field : type->fields) {
                RecordTypeAST::Field clonedField;
                clonedField.name = field.name;
                clonedField.type = cloneType(field.type.get());
                clonedField.sourcePath = field.sourcePath;
                clonedField.line = field.line;
                clonedField.col = field.col;
                if (!clonedField.type) return nullptr;
                clone->fields.push_back(std::move(clonedField));
            }
            return located(std::move(clone), src);
        }
        if (auto* type = dynamic_cast<const NamedTypeAST*>(src)) {
            auto clone = std::make_unique<NamedTypeAST>(type->name);
            clone->arrayLength = type->arrayLength;
            clone->resolvedType = substitute(type->resolvedType);
            for (const auto& argument : type->typeArgs) {
                auto clonedArgument = cloneType(argument.get());
                if (!clonedArgument) return nullptr;
                clone->typeArgs.push_back(std::move(clonedArgument));
            }
            if (auto bound = bindings.find(type->name);
                bound != bindings.end()) {
                clone->name = bound->second->toString();
                clone->resolvedType = bound->second;
                clone->typeArgs.clear();
                clone->arrayLength.reset();
            }
            return located(std::move(clone), src);
        }
        if (auto* type = dynamic_cast<const RefTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<RefTypeAST>(
                    std::move(inner), type->isMutable),
                src);
        }
        if (auto* type = dynamic_cast<const LinearTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<LinearTypeAST>(std::move(inner)), src);
        }
        if (auto* type = dynamic_cast<const AffineTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<AffineTypeAST>(std::move(inner)), src);
        }
        if (auto* type = dynamic_cast<const FunctionTypeAST*>(src)) {
            auto clone = std::make_unique<FunctionTypeAST>();
            for (const auto& parameter : type->paramTypes) {
                auto clonedParameter = cloneType(parameter.get());
                if (!clonedParameter) return nullptr;
                clone->paramTypes.push_back(std::move(clonedParameter));
            }
            clone->returnType = cloneType(type->returnType.get());
            if (type->returnType && !clone->returnType) return nullptr;
            return located(std::move(clone), src);
        }
        fail("type");
        return nullptr;
    }

    Param cloneParam(const Param& source) {
        Param clone;
        clone.name = source.name;
        clone.isLinear = source.isLinear;
        clone.usage = source.usage;
        clone.hasExplicitUsage = source.hasExplicitUsage;
        clone.relation = source.relation;
        clone.type = cloneType(source.type.get());
        clone.inferredType = substitute(source.inferredType);
        return clone;
    }

    std::unique_ptr<Expr> cloneExpr(const Expr* src) {
        if (!src) return nullptr;
        if (auto* expr = dynamic_cast<const IntLiteralExpr*>(src))
            return located(
                std::make_unique<IntLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const FloatLiteralExpr*>(src))
            return located(
                std::make_unique<FloatLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const StringLiteralExpr*>(src))
            return located(
                std::make_unique<StringLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const BoolLiteralExpr*>(src))
            return located(
                std::make_unique<BoolLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const IdentifierExpr*>(src))
            return located(
                std::make_unique<IdentifierExpr>(expr->name), src);
        if (auto* expr = dynamic_cast<const BinaryExpr*>(src)) {
            auto clone = std::make_unique<BinaryExpr>();
            clone->lhs = cloneExpr(expr->lhs.get());
            clone->op = expr->op;
            clone->rhs = cloneExpr(expr->rhs.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const UnaryExpr*>(src)) {
            auto clone = std::make_unique<UnaryExpr>();
            clone->op = expr->op;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const CallExpr*>(src)) {
            auto clone = std::make_unique<CallExpr>();
            clone->callee = cloneExpr(expr->callee.get());
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            for (const auto& type : expr->typeArgs)
                clone->typeArgs.push_back(substitute(type));
            for (const auto& type : expr->typeArgASTs)
                clone->typeArgASTs.push_back(cloneType(type.get()));
            clone->resolvedSymbolName = expr->resolvedSymbolName;
            clone->returnsLinear = expr->returnsLinear;
            clone->returnUsage = expr->returnUsage;
            clone->intrinsicType = substitute(expr->intrinsicType);
            clone->resultType = substitute(expr->resultType);
            clone->iteratorInputType = substitute(expr->iteratorInputType);
            clone->iteratorOutputType = substitute(expr->iteratorOutputType);
            clone->iteratorOp = expr->iteratorOp;
            clone->iteratorRecipeStateName =
                expr->iteratorRecipeStateName;
            clone->iteratorRecipeSourceType =
                substitute(expr->iteratorRecipeSourceType);
            clone->iteratorCollectTargetType =
                substitute(expr->iteratorCollectTargetType);
            clone->iteratorCollectBuilderType =
                substitute(expr->iteratorCollectBuilderType);
            clone->iteratorCollectBeginSymbol =
                expr->iteratorCollectBeginSymbol;
            clone->iteratorCollectPushSymbol =
                expr->iteratorCollectPushSymbol;
            clone->iteratorCollectFinishSymbol =
                expr->iteratorCollectFinishSymbol;
            clone->compileTimeValue = expr->compileTimeValue;
            clone->compileTimeDeclarationId =
                expr->compileTimeDeclarationId;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const LaunchExpr*>(src)) {
            auto clone = std::make_unique<LaunchExpr>();
            clone->kernelName = expr->kernelName;
            clone->resolvedKernelName = expr->resolvedKernelName;
            clone->threads = cloneExpr(expr->threads.get());
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->inFlightResources = expr->inFlightResources;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const VariantConstructExpr*>(src)) {
            auto clone = std::make_unique<VariantConstructExpr>();
            clone->typeName = expr->typeName;
            clone->typeSourcePath = expr->typeSourcePath;
            clone->typeLine = expr->typeLine;
            clone->typeCol = expr->typeCol;
            clone->variantName = expr->variantName;
            for (const auto& type : expr->typeArgs)
                clone->typeArgs.push_back(cloneType(type.get()));
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->constructedType = substitute(expr->constructedType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const FieldAccessExpr*>(src)) {
            auto clone = std::make_unique<FieldAccessExpr>();
            clone->object = cloneExpr(expr->object.get());
            clone->field = expr->field;
            clone->resultType = substitute(expr->resultType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const IndexExpr*>(src)) {
            auto clone = std::make_unique<IndexExpr>();
            clone->object = cloneExpr(expr->object.get());
            clone->index = cloneExpr(expr->index.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const ArrayLiteralExpr*>(src)) {
            auto clone = std::make_unique<ArrayLiteralExpr>();
            for (const auto& element : expr->elements)
                clone->elements.push_back(cloneExpr(element.get()));
            clone->elementType = substitute(expr->elementType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const RecordLiteralExpr*>(src)) {
            auto clone = std::make_unique<RecordLiteralExpr>();
            clone->recordType = substitute(expr->recordType);
            clone->targetType = cloneType(expr->targetType.get());
            for (const auto& field : expr->fields) {
                RecordLiteralExpr::Field clonedField;
                clonedField.name = field.name;
                clonedField.value = cloneExpr(field.value.get());
                clonedField.sourcePath = field.sourcePath;
                clonedField.line = field.line;
                clonedField.col = field.col;
                if (!clonedField.value) return nullptr;
                clone->fields.push_back(std::move(clonedField));
            }
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const HeapAllocExpr*>(src)) {
            auto clone = std::make_unique<HeapAllocExpr>();
            clone->initializer = cloneExpr(expr->initializer.get());
            clone->allocatedTypeAST =
                cloneType(expr->allocatedTypeAST.get());
            clone->allocatedType = substitute(expr->allocatedType);
            clone->resultType = substitute(expr->resultType);
            clone->storage = expr->storage;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const TryExpr*>(src)) {
            auto clone = std::make_unique<TryExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            clone->resultType = substitute(expr->resultType);
            clone->propagatedResultType =
                substitute(expr->propagatedResultType);
            clone->valueType = substitute(expr->valueType);
            clone->errorType = substitute(expr->errorType);
            clone->propagatedErrorType =
                substitute(expr->propagatedErrorType);
            clone->errorConversionSymbol =
                expr->errorConversionSymbol;
            for (const auto& cleanup : expr->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const MoveExpr*>(src)) {
            auto clone = std::make_unique<MoveExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const BorrowExpr*>(src)) {
            auto clone = std::make_unique<BorrowExpr>();
            clone->isMutable = expr->isMutable;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const DerefExpr*>(src)) {
            auto clone = std::make_unique<DerefExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            clone->resultType = substitute(expr->resultType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const AddrOfExpr*>(src)) {
            auto clone = std::make_unique<AddrOfExpr>();
            clone->isMutable = expr->isMutable;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const BlockExpr*>(src)) {
            auto block = cloneBlock(expr->block.get());
            if (!block) return nullptr;
            return located(
                std::make_unique<BlockExpr>(std::move(block)), src);
        }
        if (auto* expr = dynamic_cast<const IfExpr*>(src)) {
            auto clone = std::make_unique<IfExpr>();
            clone->cond = cloneExpr(expr->cond.get());
            clone->thenExpr = cloneExpr(expr->thenExpr.get());
            clone->elseExpr = cloneExpr(expr->elseExpr.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const LambdaExpr*>(src)) {
            auto clone = std::make_unique<LambdaExpr>();
            for (const auto& parameter : expr->params)
                clone->params.push_back(cloneParam(parameter));
            clone->returnType = cloneType(expr->returnType.get());
            clone->body = cloneBlock(expr->body.get());
            clone->closureType = substitute(expr->closureType);
            clone->captures = expr->captures;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const AssignExpr*>(src)) {
            auto clone = std::make_unique<AssignExpr>();
            clone->op = expr->op;
            clone->lhs = cloneExpr(expr->lhs.get());
            clone->rhs = cloneExpr(expr->rhs.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const SelectExpr*>(src)) {
            auto clone = std::make_unique<SelectExpr>();
            clone->targetName = expr->targetName;
            clone->selectorName = expr->selectorName;
            for (const auto& argument : expr->selectorArgs)
                clone->selectorArgs.push_back(cloneExpr(argument.get()));
            clone->isDynamic = expr->isDynamic;
            clone->resolvedDeclarationId =
                expr->resolvedDeclarationId;
            clone->resolvedSymbolName = expr->resolvedSymbolName;
            clone->resolvedFamilyId = expr->resolvedFamilyId;
            clone->resolvedSelectorDeclarationId =
                expr->resolvedSelectorDeclarationId;
            clone->dynamicCandidateIds = expr->dynamicCandidateIds;
            clone->dynamicMetadataSchemaId =
                expr->dynamicMetadataSchemaId;
            clone->dynamicFilterArguments =
                expr->dynamicFilterArguments;
            clone->dynamicCandidates = expr->dynamicCandidates;
            clone->selectedType = substitute(expr->selectedType);
            return located(std::move(clone), src);
        }
        fail("expression");
        return nullptr;
    }

    std::unique_ptr<Stmt> cloneStmt(const Stmt* src) {
        if (!src) return nullptr;
        if (auto* statement = dynamic_cast<const BlockStmt*>(src))
            return cloneBlock(statement);
        if (auto* statement = dynamic_cast<const LetStmt*>(src)) {
            auto clone = std::make_unique<LetStmt>();
            clone->name = statement->name;
            clone->isConst = statement->isConst;
            clone->isLinear = statement->isLinear;
            clone->usage = statement->usage;
            clone->hasExplicitUsage = statement->hasExplicitUsage;
            clone->hasInheritedUsage = statement->hasInheritedUsage;
            clone->inheritedUsage = statement->inheritedUsage;
            clone->usageResolved = statement->usageResolved;
            clone->typeAnnotation =
                cloneType(statement->typeAnnotation.get());
            clone->initializer =
                cloneExpr(statement->initializer.get());
            clone->inferredType = substitute(statement->inferredType);
            clone->materializesIteratorRecipe =
                statement->materializesIteratorRecipe;
            clone->materializedIteratorOwnsSource =
                statement->materializedIteratorOwnsSource;
            clone->materializedIteratorSourceType =
                substitute(statement->materializedIteratorSourceType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ReturnStmt*>(src)) {
            auto clone = std::make_unique<ReturnStmt>();
            clone->value = cloneExpr(statement->value.get());
            clone->autoFrees = statement->autoFrees;
            for (const auto& cleanup : statement->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ExprStmt*>(src)) {
            auto clone = std::make_unique<ExprStmt>();
            clone->expr = cloneExpr(statement->expr.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const IfStmt*>(src)) {
            auto clone = std::make_unique<IfStmt>();
            clone->cond = cloneExpr(statement->cond.get());
            clone->thenBlock = cloneBlock(statement->thenBlock.get());
            clone->elseBranch = cloneStmt(statement->elseBranch.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const MatchStmt*>(src)) {
            auto clone = std::make_unique<MatchStmt>();
            clone->scrutinee = cloneExpr(statement->scrutinee.get());
            clone->matchedType = substitute(statement->matchedType);
            for (const auto& sourceArm : statement->arms) {
                MatchArm arm;
                arm.sourcePath = sourceArm.sourcePath;
                arm.line = sourceArm.line;
                arm.col = sourceArm.col;
                arm.typeQualifier = sourceArm.typeQualifier;
                arm.qualifierLine = sourceArm.qualifierLine;
                arm.qualifierCol = sourceArm.qualifierCol;
                arm.variantName = sourceArm.variantName;
                arm.bindings = sourceArm.bindings;
                arm.bindingUsageDefaults =
                    sourceArm.bindingUsageDefaults;
                arm.bindingUsages = sourceArm.bindingUsages;
                arm.variantIndex = sourceArm.variantIndex;
                for (const auto& type : sourceArm.bindingTypes)
                    arm.bindingTypes.push_back(substitute(type));
                arm.body = cloneBlock(sourceArm.body.get());
                clone->arms.push_back(std::move(arm));
            }
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const WhileStmt*>(src)) {
            auto clone = std::make_unique<WhileStmt>();
            clone->cond = cloneExpr(statement->cond.get());
            clone->body = cloneBlock(statement->body.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ForStmt*>(src)) {
            auto clone = std::make_unique<ForStmt>();
            clone->varName = statement->varName;
            clone->hasInheritedUsage =
                statement->hasInheritedUsage;
            clone->inheritedUsage =
                statement->inheritedUsage;
            clone->bindingUsage = statement->bindingUsage;
            clone->iterable = cloneExpr(statement->iterable.get());
            clone->body = cloneBlock(statement->body.get());
            clone->elementType = substitute(statement->elementType);
            clone->protocolNextSymbol = statement->protocolNextSymbol;
            clone->protocolIteratorType =
                substitute(statement->protocolIteratorType);
            clone->protocolOptionType =
                substitute(statement->protocolOptionType);
            clone->protocolNoneVariant =
                statement->protocolNoneVariant;
            clone->protocolSomeVariant =
                statement->protocolSomeVariant;
            clone->protocolIntoSymbol = statement->protocolIntoSymbol;
            clone->protocolInputType =
                substitute(statement->protocolInputType);
            clone->protocolStateName = statement->protocolStateName;
            clone->protocolStateNeedsCleanup =
                statement->protocolStateNeedsCleanup;
            clone->protocolStateCleanup =
                statement->protocolStateCleanup;
            clone->recipeStateName = statement->recipeStateName;
            clone->recipeSourceType =
                substitute(statement->recipeSourceType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const FreeStmt*>(src)) {
            auto clone = std::make_unique<FreeStmt>();
            clone->operand = cloneExpr(statement->operand.get());
            clone->action = statement->action;
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const SlotDeclStmt*>(src)) {
            auto clone = std::make_unique<SlotDeclStmt>();
            clone->name = statement->name;
            clone->acceptedKind = statement->acceptedKind;
            clone->acceptedCardinality =
                statement->acceptedCardinality;
            clone->isDynamic = statement->isDynamic;
            for (const auto& parameter : statement->params)
                clone->params.push_back(cloneParam(parameter));
            clone->defaultFragment = statement->defaultFragment;
            clone->resolvedDefaultFragmentName =
                statement->resolvedDefaultFragmentName;
            clone->structuralType =
                substitute(statement->structuralType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const SlotInvokeStmt*>(src)) {
            auto clone = std::make_unique<SlotInvokeStmt>();
            clone->name = statement->name;
            clone->acceptedKind = statement->acceptedKind;
            clone->acceptedCardinality =
                statement->acceptedCardinality;
            clone->isDynamic = statement->isDynamic;
            clone->usesDynamicDispatch =
                statement->usesDynamicDispatch;
            clone->resolvedDynamicFragmentNames =
                statement->resolvedDynamicFragmentNames;
            for (const auto& argument : statement->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->continuation =
                cloneBlock(statement->continuation.get());
            clone->isImplicitCapture =
                statement->isImplicitCapture;
            for (const auto& parameter : statement->interfaceParams)
                clone->interfaceParams.push_back(cloneParam(parameter));
            clone->resolvedParamNames =
                statement->resolvedParamNames;
            clone->defaultFragment = statement->defaultFragment;
            clone->resolvedDefaultFragmentName =
                statement->resolvedDefaultFragmentName;
            clone->structuralType =
                substitute(statement->structuralType);
            return located(std::move(clone), src);
        }
        if (dynamic_cast<const ResumeStmt*>(src))
            return located(std::make_unique<ResumeStmt>(), src);
        if (auto* statement = dynamic_cast<const AbortStmt*>(src)) {
            auto clone = std::make_unique<AbortStmt>();
            clone->autoFrees = statement->autoFrees;
            for (const auto& cleanup : statement->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const AwaitStmt*>(src)) {
            auto clone = std::make_unique<AwaitStmt>();
            clone->event = cloneExpr(statement->event.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ApplyStmt*>(src)) {
            auto clone = std::make_unique<ApplyStmt>();
            clone->slotName = statement->slotName;
            clone->fragmentName = statement->fragmentName;
            clone->isDynamic = statement->isDynamic;
            clone->alternativeFragmentNames =
                statement->alternativeFragmentNames;
            clone->resolvedAlternativeFragmentNames =
                statement->resolvedAlternativeFragmentNames;
            clone->resolvedFragmentName =
                statement->resolvedFragmentName;
            clone->body = cloneBlock(statement->body.get());
            return located(std::move(clone), src);
        }
        fail("statement");
        return nullptr;
    }
};

FunctionDecl* TypeResolver::monomorphize(FunctionDecl* generic, const TypeVec& concreteTypes) {
    luna::instantiation::Request request;
    const auto genericSymbol = generic->generatedSymbolName.empty()
        ? generic->name : generic->generatedSymbolName;
    request.genericDeclarationId = nominalDeclarationIdentity(
        mContext.mProgram, "fn", genericSymbol, generic);
    for (const auto& type : concreteTypes)
        request.typeArguments.push_back(mContext.typeIdentity(resolved(type)));
    request.requestedBy = mContext.mDiagnosticFile + ":" +
        std::to_string(mContext.mDiagnosticLine) + ":" + std::to_string(mContext.mDiagnosticCol);
    const std::string requestKey =
        luna::instantiation::Instantiator::keyFor(request);
    auto cached = mContext.mInstantiatedFunctions.find(requestKey);
    if (cached != mContext.mInstantiatedFunctions.end()) return cached->second;

    const auto& entry = mContext.mInstantiator.begin(request);
    if (entry.state == luna::instantiation::State::Failed) {
        mContext.error("generic instantiation previously failed: " + entry.failure);
        return nullptr;
    }
    auto specialized = std::make_unique<FunctionDecl>();
    specialized->name = entry.instanceId;
    specialized->generatedSymbolName = entry.instanceId;
    specialized->sourcePath = generic->sourcePath;
    specialized->line = generic->line;
    specialized->col = generic->col;
    specialized->packageId = generic->packageId;
    specialized->modulePath = generic->modulePath;
    specialized->isExported = generic->isExported;
    specialized->isKernel = generic->isKernel;
    specialized->isConstexpr = generic->isConstexpr;
    specialized->returnsLinear = generic->returnsLinear;
    specialized->returnUsage = generic->returnUsage;
    specialized->isTemplateInstance = true;
    specialized->concreteTypeArgs = concreteTypes;

    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < generic->typeParams.size() && i < concreteTypes.size(); ++i) {
        bindings[generic->typeParams[i]] = concreteTypes[i];
    }

    for (auto& p : generic->params) {
        Param cp;
        cp.name = p.name;
        cp.isLinear = p.isLinear;
        cp.usage = p.usage;
        cp.relation = p.relation;
        cp.hasExplicitUsage = p.hasExplicitUsage;
        TypePtr parameterType = p.inferredType
            ? substituteNominalType(
                  mContext.mConstraints.resolve(p.inferredType), bindings)
            : (p.type ? resolveType(p.type.get(), bindings)
                      : TyUnknown);
        if (parameterType && parameterType->kind == TypeKind::TypeParam) {
            auto it = bindings.find(parameterType->name);
            if (it != bindings.end()) parameterType = it->second;
        }
        cp.inferredType = parameterType;
        cp.type = typeToAST(parameterType);
        specialized->params.push_back(std::move(cp));
    }

    if (generic->returnType) {
        const TypePtr returnType = generic->inferredReturnType
            ? substituteNominalType(
                  mContext.mConstraints.resolve(generic->inferredReturnType),
                  bindings)
            : resolveType(generic->returnType.get(), bindings);
        specialized->returnType = typeToAST(returnType);
        if (specialized->returnsLinear)
            specialized->returnType = std::make_unique<LinearTypeAST>(
                std::move(specialized->returnType));
        else if (generic->returnUsage == luna::ownership::Usage::Affine)
            specialized->returnType = std::make_unique<AffineTypeAST>(
                std::move(specialized->returnType));
        }

    if (generic->body) {
        MonomorphizationCloner cloner(bindings);
        specialized->body = cloner.cloneBlock(generic->body.get());
        if (!specialized->body || !cloner.failure().empty()) {
            const std::string failure = cloner.failure().empty()
                ? "generic function body could not be cloned"
                : cloner.failure();
            mContext.mInstantiator.fail(requestKey, failure);
            mContext.error("cannot instantiate generic function '" + generic->name +
                  "': " + failure);
            return nullptr;
        }
    } else {
        specialized->body = std::make_unique<BlockStmt>();
    }

    mContext.mGeneratedInstances.push_back(std::move(specialized));
    auto* result = mContext.mGeneratedInstances.back().get();
    mContext.mInstantiatedFunctions[requestKey] = result;
    if (!mContext.mInstantiator.complete(requestKey)) {
        mContext.mInstantiator.fail(requestKey, "instance state transition failed");
        mContext.error("generic instantiator could not finalize '" + entry.instanceId + "'");
        return nullptr;
    }
    return result;
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
        auto resolvedRecord = Type::makeRecord(std::move(fields));
        const_cast<RecordTypeAST*>(record)->resolvedType = resolvedRecord;
        return resolvedRecord;
    }
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        // Monomorphization may materialize an already-resolved nominal type
        // into an AST annotation. Preserve that exact identity instead of
        // resolving its display name through the declaration family again.
        if (named->resolvedType) return named->resolvedType;
        auto bound = bindings.find(named->name);
        if (bound != bindings.end()) return bound->second;
        if (auto* symbol = mContext.mSymTable.lookup(named->name);
            symbol && symbol->kind == SymbolKind::TypeParam && symbol->type)
            return symbol->type;
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
        if (named->name == "never") return TyNever;
        if (named->name == "Self") return Type::makeTypeParam("Self");

        if (named->name == "raw") {
            if (named->typeArgs.size() != 1) {
                mContext.error("raw<T> requires exactly one type argument");
                return TyUnknown;
            }
            return Type::makeRawPointer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "Result") {
            if (named->typeArgs.size() != 2) {
                mContext.error("Result<T, E> requires value and error type arguments",
                      named->line, named->col);
                return TyUnknown;
            }
            return Type::makeResult(
                resolveTypeAST(named->typeArgs[0].get(), bindings),
                resolveTypeAST(named->typeArgs[1].get(), bindings));
        }
        if (named->name == "device_buffer") {
            if (named->typeArgs.size() != 1) {
                mContext.error("device_buffer<T> requires exactly one element type");
                return TyUnknown;
            }
            return Type::makeDeviceBuffer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "array") {
            if (named->typeArgs.size() != 1 || !named->arrayLength) {
                mContext.error("array<T, N> requires one element type and a non-negative compile-time length",
                      named->line, named->col);
                return TyUnknown;
            }
            return Type::makeArray(resolveTypeAST(named->typeArgs[0].get(), bindings),
                                   *named->arrayLength);
        }
        if (named->name == "slice") {
            if (named->typeArgs.size() != 1) { mContext.error("slice<T> requires exactly one element type", named->line, named->col); return TyUnknown; }
            return Type::makeSlice(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "event") return TyEvent;
        if (named->name == "metadata_view") {
            if (named->typeArgs.size() != 1) {
                mContext.error("metadata_view<M> requires exactly one metadata type",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr metadata = resolveTypeAST(
                named->typeArgs.front().get(), bindings);
            if (resolved(metadata)->kind != TypeKind::Metadata)
                mContext.error("metadata_view type argument must be a meta schema",
                      named->line, named->col);
            return Type::makeMetadataView(metadata);
        }
        if (named->name == "declaration_view") {
            if (named->typeArgs.size() > 1) {
                mContext.error("declaration_view accepts at most one callable type argument",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr callable = named->typeArgs.empty()
                ? nullptr : resolveTypeAST(named->typeArgs.front().get(), bindings);
            return Type::makeDeclarationView(callable);
        }
        if (named->name == "declaration_ref") {
            if (named->typeArgs.size() > 1) {
                mContext.error("declaration_ref accepts at most one callable type argument",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr callable = named->typeArgs.empty()
                ? nullptr : resolveTypeAST(named->typeArgs.front().get(), bindings);
            return Type::makeDeclarationRef(callable);
        }
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
            const_cast<NamedTypeAST*>(named)->resolvedType = args.empty()
                ? nominalType : instantiateNominal(nominalType, args);
            return const_cast<NamedTypeAST*>(named)->resolvedType;
        }
        return resolveType(ast, bindings);
    }
    if (auto* ref = dynamic_cast<const RefTypeAST*>(ast))
        return Type::makeReference(resolveTypeAST(ref->inner.get(), bindings),
                                   ref->isMutable);
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
        return Type::makeFunction(
            std::move(params), std::move(returnType), std::move(contracts),
            {luna::ownership::Relation::Owned, returnUsage});
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

    visitExpr = [&](Expr* expr) {
        if (!expr) return;
        if (auto* l = dynamic_cast<LambdaExpr*>(expr)) {
            for (auto& p : l->params) {
                checkUnresolved(p.inferredType, "lambda parameter '" + p.name + "'");
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
            }
            checkUnresolved(l->closureType, "lambda type");
            l->closureType = resolved(l->closureType);
            if (needsConcreteAnnotation(l->returnType) && l->closureType &&
                l->closureType->kind == TypeKind::Function)
                l->returnType = typeToAST(l->closureType->returnType);
            visitBlock(l->body.get());
            return;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(expr)) { visitExpr(b->lhs.get()); visitExpr(b->rhs.get()); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(expr)) { visitExpr(u->operand.get()); return; }
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
        }
    }
}
