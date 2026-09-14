#include "Lowering.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"
#include "../lexer/Token.h"
#include "../parser/AST.h"
#include "../sema/SemanticAnalysisSupport.h"
#include "../sema/SymbolTable.h"

#include <algorithm>
#include <tuple>

namespace moon {

std::unique_ptr<moon::Expr> LunaLowerer::lowerExpr(const ::Expr* expression) {
    if (!expression) return nullptr;
    std::unique_ptr<moon::Expr> result;
    if (auto* literal = dynamic_cast<const ::IntLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::IntLiteralExpr>();
        value->value = literal->value;
        value->type = typeRef(literal->inferredType
            ? literal->inferredType : TyI32);
        result = std::move(value);
    } else if (auto* literal = dynamic_cast<const ::FloatLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::FloatLiteralExpr>();
        value->value = literal->value;
        value->type = typeRef(TyF64);
        result = std::move(value);
    } else if (auto* literal = dynamic_cast<const ::StringLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::StringLiteralExpr>();
        value->value = literal->value;
        value->type = typeRef(TyString);
        result = std::move(value);
    } else if (auto* literal = dynamic_cast<const ::BoolLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::BoolLiteralExpr>();
        value->value = literal->value;
        value->type = typeRef(TyBool);
        result = std::move(value);
    } else if (auto* identifier = dynamic_cast<const ::IdentifierExpr*>(expression)) {
        auto value = std::make_unique<moon::IdentifierExpr>();
        const auto compileTime =
            mCompileTimeDeclarationBindings.find(identifier->name);
        if (compileTime != mCompileTimeDeclarationBindings.end()) {
            value->name = compileTime->second.symbolName;
            value->type = typeRef(compileTime->second.type);
            deferDeclarationRef(
                value->declaration,
                compileTime->second.declarationId,
                identifier, "compile-time declaration_view element",
                true, compileTime->second.symbolId,
                compileTime->second.contractId);
        } else {
            value->name = identifier->name;
            deferDeclarationRef(
                value->declaration, identifier->resolvedSymbolName,
                identifier, "declaration-valued identifier");
        }
        result = std::move(value);
    } else if (auto* binary = dynamic_cast<const ::BinaryExpr*>(expression)) {
        auto value = std::make_unique<moon::BinaryExpr>();
        value->lhs = lowerExpr(binary->lhs.get());
        value->op = lowerOperator(static_cast<int>(binary->op), binary);
        value->rhs = lowerExpr(binary->rhs.get());
        switch (value->op) {
            case Operator::Equal:
            case Operator::NotEqual:
            case Operator::Less:
            case Operator::LessEqual:
            case Operator::Greater:
            case Operator::GreaterEqual:
            case Operator::LogicalAnd:
            case Operator::LogicalOr:
                value->type = typeRef(TyBool);
                break;
            default:
                break;
        }
        result = std::move(value);
    } else if (auto* unary = dynamic_cast<const ::UnaryExpr*>(expression)) {
        auto value = std::make_unique<moon::UnaryExpr>();
        if (unary->op == TokenKind::Minus) value->op = Operator::Negate;
        else if (unary->op == TokenKind::Star) value->op = Operator::Dereference;
        else value->op = lowerOperator(static_cast<int>(unary->op), unary);
        value->operand = lowerExpr(unary->operand.get());
        if (value->op == Operator::LogicalNot)
            value->type = typeRef(TyBool);
        result = std::move(value);
    } else if (auto* call = dynamic_cast<const ::CallExpr*>(expression)) {
        if (const auto* callee = dynamic_cast<const ::IdentifierExpr*>(
                call->callee.get());
            callee && callee->name == "declaration_id" &&
            call->args.size() == 1) {
            if (const auto* identifier =
                    dynamic_cast<const ::IdentifierExpr*>(
                        call->args.front().get())) {
                const auto binding =
                    mCompileTimeDeclarationBindings.find(identifier->name);
                if (binding != mCompileTimeDeclarationBindings.end()) {
                    auto value =
                        std::make_unique<moon::StringLiteralExpr>();
                    value->value = binding->second.declarationId;
                    value->type = typeRef(TyString);
                    value->location = locationOf(expression);
                    return value;
                }
            }
        }
        // A call with a folded compile-time value (declaration_id/signature,
        // type_*, declaration_has_metadata, etc.) is erased to a literal so it
        // does not reach MoonIR as a call referencing compile-time-only
        // operands (e.g. a DeclarationRef binding that has no runtime local).
        if (call->compileTimeValue) {
            if (auto* integer = std::get_if<int64_t>(&*call->compileTimeValue)) {
                auto value = std::make_unique<moon::IntLiteralExpr>();
                value->value = *integer;
                value->type = typeRef(TyI32);
                result = std::move(value);
            } else if (auto* boolean = std::get_if<bool>(&*call->compileTimeValue)) {
                auto value = std::make_unique<moon::BoolLiteralExpr>();
                value->value = *boolean;
                value->type = typeRef(TyBool);
                result = std::move(value);
            } else if (auto* str = std::get_if<std::string>(&*call->compileTimeValue)) {
                auto value = std::make_unique<moon::StringLiteralExpr>();
                value->value = *str;
                value->type = typeRef(TyString);
                result = std::move(value);
            } else if (auto* floating = std::get_if<double>(&*call->compileTimeValue)) {
                auto value = std::make_unique<moon::FloatLiteralExpr>();
                value->value = *floating;
                value->type = typeRef(TyF64);
                result = std::move(value);
            }
            if (result) return result;
        }
        if (const auto* callee = dynamic_cast<const ::IdentifierExpr*>(
                call->callee.get());
            callee &&
            (callee->name == "Ok" || callee->name == "Err") &&
            call->args.size() == 1 && call->intrinsicType) {
            auto value = std::make_unique<moon::ResultConstructExpr>();
            value->location = locationOf(expression);
            value->type = typeRef(call->intrinsicType);
            value->isOk = callee->name == "Ok";
            value->payload = lowerExpr(call->args.front().get());
            const size_t payloadIndex = value->isOk ? 0 : 1;
            if (value->payload && value->payload->type.empty() &&
                call->intrinsicType->typeArgs.size() == 2)
                value->payload->type = typeRef(
                    call->intrinsicType->typeArgs[payloadIndex]);
            return value;
        }
        auto value = std::make_unique<moon::CallExpr>();
        const CompileTimeDeclarationBinding* loopBinding = nullptr;
        if (const auto* identifier = dynamic_cast<const ::IdentifierExpr*>(
                call->callee.get())) {
            const auto found =
                mCompileTimeDeclarationBindings.find(identifier->name);
            if (found != mCompileTimeDeclarationBindings.end())
                loopBinding = &found->second;
        }
        if (loopBinding) {
            auto callee = std::make_unique<moon::IdentifierExpr>();
            callee->name = loopBinding->symbolName;
            callee->type = typeRef(loopBinding->type);
            deferDeclarationRef(
                callee->declaration, loopBinding->declarationId,
                call, "compile-time declaration_view call target", true,
                loopBinding->symbolId, loopBinding->contractId);
            value->callee = std::move(callee);
            deferDeclarationRef(
                value->calleeRef, loopBinding->declarationId,
                call, "compile-time declaration_view call target", true,
                loopBinding->symbolId, loopBinding->contractId);
        } else if (const auto* compileTimeReference =
                dynamic_cast<const ::CallExpr*>(call->callee.get());
            compileTimeReference &&
            !compileTimeReference->compileTimeDeclarationId.empty() &&
            !call->resolvedSymbolName.empty()) {
            auto callee = std::make_unique<moon::IdentifierExpr>();
            callee->name = call->resolvedSymbolName;
            deferDeclarationRef(
                callee->declaration, call->resolvedSymbolName,
                call, "compile-time query call target", false,
                call->resolvedSymbolId,
                call->resolvedContractId);
            if (compileTimeReference->resultType &&
                compileTimeReference->resultType->inner)
                callee->type = typeRef(
                    compileTimeReference->resultType->inner);
            value->callee = std::move(callee);
        } else {
            value->callee = lowerExpr(call->callee.get());
        }
        for (const auto& argument : call->args)
            value->args.push_back(lowerExpr(argument.get()));
        value->typeArgs = typeRefs(call->typeArgs);
        deferDeclarationRef(value->calleeRef, call->resolvedSymbolName,
                            call, "call target", false,
                            call->resolvedSymbolId,
                            call->resolvedContractId);
        if (!call->resolvedSymbolName.empty()) {
            if (auto* callee = dynamic_cast<moon::IdentifierExpr*>(
                    value->callee.get()))
                deferDeclarationRef(
                    callee->declaration, call->resolvedSymbolName,
                    call, "resolved call identifier", false,
                    call->resolvedSymbolId,
                    call->resolvedContractId);
        }
        value->returnsLinear = call->returnsLinear;
        value->returnUsage = call->returnUsage;
        value->intrinsicType = typeRef(call->intrinsicType);
        value->iteratorInputType = typeRef(call->iteratorInputType);
        value->iteratorOutputType = typeRef(call->iteratorOutputType);
        value->iteratorOp = call->iteratorOp;
        value->iteratorRecipeStateName =
            call->iteratorRecipeStateName;
        value->iteratorRecipeSourceType =
            typeRef(call->iteratorRecipeSourceType);
        value->iteratorCollectTargetType =
            typeRef(call->iteratorCollectTargetType);
        value->iteratorCollectBuilderType =
            typeRef(call->iteratorCollectBuilderType);
        deferDeclarationRef(
            value->iteratorCollectBegin,
            call->iteratorCollectBeginSymbol,
            call, "FromIterator begin witness");
        deferDeclarationRef(
            value->iteratorCollectPush,
            call->iteratorCollectPushSymbol,
            call, "FromIterator push witness");
        deferDeclarationRef(
            value->iteratorCollectFinish,
            call->iteratorCollectFinishSymbol,
            call, "FromIterator finish witness");
        value->compileTimeValue = call->compileTimeValue;
        if (call->resultType) {
            value->type = typeRef(call->resultType);
        } else if (call->intrinsicType) {
            if (auto* callee = dynamic_cast<const ::IdentifierExpr*>(
                    call->callee.get())) {
                if (callee->name == "Ok" || callee->name == "Err")
                    value->type = typeRef(call->intrinsicType);
                else if (callee->name == "is_ok" || callee->name == "is_err")
                    value->type = typeRef(TyBool);
                else if (callee->name == "unwrap" &&
                         call->intrinsicType->typeArgs.size() == 2)
                    value->type = typeRef(call->intrinsicType->typeArgs[0]);
                else if (callee->name == "unwrap_err" &&
                         call->intrinsicType->typeArgs.size() == 2)
                    value->type = typeRef(call->intrinsicType->typeArgs[1]);
                else if (callee->name == "panic")
                    value->type = typeRef(TyUnit);
            }
        }
        result = std::move(value);
    } else if (auto* launch = dynamic_cast<const ::LaunchExpr*>(expression)) {
        auto value = std::make_unique<moon::LaunchExpr>();
        value->kernelName = launch->kernelName;
        deferDeclarationRef(value->kernelRef, launch->resolvedKernelName,
                            launch, "kernel target");
        value->threads = lowerExpr(launch->threads.get());
        for (const auto& argument : launch->args)
            value->args.push_back(lowerExpr(argument.get()));
        value->inFlightResources = launch->inFlightResources;
        value->type = typeRef(TyEvent);
        mModule->features.kernel = true;
        mRequiredKernelSymbols.insert(launch->resolvedKernelName);
        result = std::move(value);
    } else if (auto* variant = dynamic_cast<const ::VariantConstructExpr*>(expression)) {
        auto value = std::make_unique<moon::VariantConstructExpr>();
        value->typeName = variant->typeName;
        value->variantName = variant->variantName;
        for (const auto& argument : variant->args)
            value->args.push_back(lowerExpr(argument.get()));
        value->constructedType = typeRef(variant->constructedType);
        value->type = value->constructedType;
        result = std::move(value);
    } else if (auto* field = dynamic_cast<const ::FieldAccessExpr*>(expression)) {
        auto value = std::make_unique<moon::FieldAccessExpr>();
        value->object = lowerExpr(field->object.get());
        value->field = field->field;
        value->type = typeRef(field->resultType);
        const TypeRecord* objectType = value->object
            ? mModule->findType(value->object->type) : nullptr;
        if (objectType && objectType->kind == TypeKind::Reference)
            objectType = mModule->findType(objectType->innerTypeId);
        if (objectType) {
            for (const auto& candidate : objectType->fields) {
                if (candidate.name == field->field) {
                    value->type = candidate.type;
                    break;
                }
            }
        }
        result = std::move(value);
    } else if (auto* index = dynamic_cast<const ::IndexExpr*>(expression)) {
        auto value = std::make_unique<moon::IndexExpr>();
        value->object = lowerExpr(index->object.get());
        value->index = lowerExpr(index->index.get());
        result = std::move(value);
    } else if (auto* array = dynamic_cast<const ::ArrayLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::ArrayLiteralExpr>();
        value->elements.reserve(array->elements.size());
        for (const auto& element : array->elements)
            value->elements.push_back(lowerExpr(element.get()));
        value->elementType = typeRef(array->elementType);
        value->type = typeRef(
            Type::makeArray(array->elementType, array->elements.size()));
        result = std::move(value);
    } else if (auto* record = dynamic_cast<const ::RecordLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::RecordLiteralExpr>();
        for (const auto& field : record->fields) {
            moon::RecordLiteralExpr::Field loweredField;
            loweredField.name = field.name;
            loweredField.value = lowerExpr(field.value.get());
            value->fields.push_back(std::move(loweredField));
        }
        value->type = typeRef(record->recordType);
        result = std::move(value);
    } else if (auto* allocation = dynamic_cast<const ::HeapAllocExpr*>(expression)) {
        auto value = std::make_unique<moon::HeapAllocExpr>();
        value->initializer = lowerExpr(allocation->initializer.get());
        value->allocatedType = typeRef(allocation->allocatedType);
        value->storage = allocation->storage;
        value->type = typeRef(allocation->resultType
            ? allocation->resultType
            : Type::makeRawPointer(allocation->allocatedType));
        result = std::move(value);
    } else if (auto* propagation = dynamic_cast<const ::TryExpr*>(expression)) {
        auto value = std::make_unique<moon::TryExpr>();
        value->operand = lowerExpr(propagation->operand.get());
        value->resultType = typeRef(propagation->resultType);
        value->propagatedResultType = typeRef(propagation->propagatedResultType);
        value->valueType = typeRef(propagation->valueType);
        value->errorType = typeRef(propagation->errorType);
        value->propagatedErrorType = typeRef(propagation->propagatedErrorType);
        deferDeclarationRef(
            value->errorConversion,
            propagation->errorConversionSymbol,
            propagation, "From error conversion");
        value->type = value->valueType;
        for (const auto& cleanup : propagation->cleanups) {
            moon::CleanupObligation lowered;
            lowered.place = cleanup.place;
            lowered.action = cleanup.action;
            lowered.typeId = cleanup.type
                ? luna::types::typeId(cleanup.type)
                : luna::types::TypeId{};
            value->cleanups.push_back(std::move(lowered));
        }
        result = std::move(value);
    } else if (auto* move = dynamic_cast<const ::MoveExpr*>(expression)) {
        auto value = std::make_unique<moon::MoveExpr>();
        value->operand = lowerExpr(move->operand.get());
        // A move preserves the operand's type; the canonical verifier checks
        // call-argument types against function signatures, so the MoveExpr
        // must carry its result type.
        if (value->operand)
            value->type = value->operand->type;
        result = std::move(value);
    } else if (auto* borrow = dynamic_cast<const ::BorrowExpr*>(expression)) {
        auto value = std::make_unique<moon::BorrowExpr>();
        value->isMutable = borrow->isMutable;
        value->operand = lowerExpr(borrow->operand.get());
        result = std::move(value);
    } else if (auto* dereference = dynamic_cast<const ::DerefExpr*>(expression)) {
        auto value = std::make_unique<moon::DerefExpr>();
        value->operand = lowerExpr(dereference->operand.get());
        value->type = typeRef(dereference->resultType);
        result = std::move(value);
    } else if (auto* address = dynamic_cast<const ::AddrOfExpr*>(expression)) {
        auto value = std::make_unique<moon::AddrOfExpr>();
        value->isMutable = address->isMutable;
        value->operand = lowerExpr(address->operand.get());
        result = std::move(value);
    } else if (auto* block = dynamic_cast<const ::BlockExpr*>(expression)) {
        auto value = std::make_unique<moon::BlockExpr>();
        value->block = lowerBlock(block->block.get());
        result = std::move(value);
    } else if (auto* conditional = dynamic_cast<const ::IfExpr*>(expression)) {
        auto value = std::make_unique<moon::IfExpr>();
        value->cond = lowerExpr(conditional->cond.get());
        value->thenExpr = lowerExpr(conditional->thenExpr.get());
        value->elseExpr = lowerExpr(conditional->elseExpr.get());
        result = std::move(value);
    } else if (auto* lambda = dynamic_cast<const ::LambdaExpr*>(expression)) {
        auto value = std::make_unique<moon::LambdaExpr>();
        for (const auto& parameter : lambda->params)
            value->params.push_back(lowerParam(parameter));
        const TypePtr returnType = lambda->returnType
            ? lowerType(lambda->returnType.get())
            : (lambda->closureType && lambda->closureType->returnType
                ? lambda->closureType->returnType : TyI32);
        value->returnType = typeRef(returnType);
        value->body = lowerBlock(lambda->body.get());
        value->closureType = typeRef(lambda->closureType);
        value->type = value->closureType;
        value->captures = lambda->captures;
        value->envParamName = lambda->envParamName;
        if (lambda->captures.empty()) {
            result = std::move(value);
        } else {
            auto closure = std::make_unique<moon::MakeClosureExpr>();
            closure->type = value->closureType;
            for (const auto& capture : lambda->captures) {
                auto reference = std::make_unique<moon::IdentifierExpr>();
                reference->name = capture;
                TypePtr captureType;
                if (lambda->closureType)
                    for (const auto& field :
                         lambda->closureType->capturedFields)
                        if (field.name == capture) {
                            captureType = field.type;
                            break;
                        }
                reference->type = typeRef(captureType);
                closure->capturedValues.push_back(
                    std::move(reference));
            }
            closure->lambda = std::move(value);
            result = std::move(closure);
        }
    } else if (auto* assignment = dynamic_cast<const ::AssignExpr*>(expression)) {
        auto value = std::make_unique<moon::AssignExpr>();
        value->op = lowerOperator(static_cast<int>(assignment->op), assignment);
        value->lhs = lowerExpr(assignment->lhs.get());
        value->rhs = lowerExpr(assignment->rhs.get());
        result = std::move(value);
    } else if (auto* selection = dynamic_cast<const ::SelectExpr*>(expression)) {
        auto value = std::make_unique<moon::IdentifierExpr>();
        value->name = selection->resolvedSymbolName;
        deferDeclarationRef(
            value->declaration, selection->resolvedDeclarationId,
            selection, "statically selected declaration", true,
            selection->resolvedSymbolId,
            selection->resolvedContractId);
        value->type = typeRef(selection->selectedType);
        result = std::move(value);
    } else {
        error(expression, "unsupported expression reached MoonIR lowering");
        return nullptr;
    }
    result->location = locationOf(expression);
    if (result->type.empty()) result->type = typeRef(inferredExprType(expression));
    return result;
}

std::unique_ptr<moon::Stmt> LunaLowerer::lowerStmt(const ::Stmt* statement) {
    if (!statement) return nullptr;
    std::unique_ptr<moon::Stmt> result;
    if (auto* block = dynamic_cast<const ::BlockStmt*>(statement)) {
        result = lowerBlock(block);
    } else if (auto* let = dynamic_cast<const ::LetStmt*>(statement)) {
        if (let->inferredType &&
            (let->inferredType->kind == TypeKind::SymbolSet ||
             let->inferredType->kind == TypeKind::DeclarationView ||
             let->inferredType->kind == TypeKind::DeclarationRef ||
             (let->inferredType->kind == TypeKind::Enum &&
              let->inferredType->domain ==
                  luna::types::TypeDomain::Compiler &&
              let->inferredType->nominalId ==
                  luna::sysmeta::OptionTypeId))) {
            if (let->inferredType->kind == TypeKind::DeclarationRef) {
                if (const auto* identifier =
                        dynamic_cast<const ::IdentifierExpr*>(
                            let->initializer.get())) {
                    const auto source =
                        mCompileTimeDeclarationBindings.find(
                            identifier->name);
                    if (source !=
                        mCompileTimeDeclarationBindings.end())
                        mCompileTimeDeclarationBindings[let->name] =
                            source->second;
                }
            }
            return nullptr;
        }
        if (let->materializesIteratorRecipe) {
            (void)typeRef(TyI32);
            (void)typeRef(TyUSize);
            (void)typeRef(TyBool);
        }
        auto value = std::make_unique<moon::LetStmt>();
        value->name = let->name;
        value->isConst = let->isConst;
        value->isLinear = let->isLinear;
        value->usage = let->usage;
        value->materializesIteratorRecipe =
            let->materializesIteratorRecipe;
        value->materializedIteratorOwnsSource =
            let->materializedIteratorOwnsSource;
        value->materializedIteratorSourceType =
            typeRef(let->materializedIteratorSourceType);
        value->initializer = lowerExpr(let->initializer.get());
        value->type = let->inferredType
            ? typeRef(let->inferredType)
            : (let->typeAnnotation
                ? typeRef(lowerType(let->typeAnnotation.get()))
                : (value->initializer ? value->initializer->type : TypeRef{}));
        result = std::move(value);
    } else if (auto* ret = dynamic_cast<const ::ReturnStmt*>(statement)) {
        auto value = std::make_unique<moon::ReturnStmt>();
        value->value = lowerExpr(ret->value.get());
        value->autoFrees = ret->autoFrees;
        for (const auto& cleanup : ret->cleanups) {
            moon::CleanupObligation lowered;
            lowered.place = cleanup.place;
            lowered.action = cleanup.action;
            if (cleanup.type) {
                if (mModule) mModule->registerType(cleanup.type);
                lowered.typeId = luna::types::typeId(cleanup.type);
            }
            value->cleanups.push_back(std::move(lowered));
        }
        result = std::move(value);
    } else if (auto* expression = dynamic_cast<const ::ExprStmt*>(statement)) {
        auto value = std::make_unique<moon::ExprStmt>();
        value->expr = lowerExpr(expression->expr.get());
        result = std::move(value);
    } else if (auto* conditional = dynamic_cast<const ::IfStmt*>(statement)) {
        auto value = std::make_unique<moon::IfStmt>();
        value->cond = lowerExpr(conditional->cond.get());
        value->thenBlock = lowerBlock(conditional->thenBlock.get());
        value->elseBranch = lowerStmt(conditional->elseBranch.get());
        result = std::move(value);
    } else if (auto* match = dynamic_cast<const ::MatchStmt*>(statement)) {
        if (match->isCompileTimeOptionalMatch) {
            if (match->compileTimeSelectedArm >= match->arms.size()) {
                error(match, "compile-time Option match has no selected arm");
                return nullptr;
            }
            return lowerBlock(
                match->arms[match->compileTimeSelectedArm].body.get());
        }
        auto value = std::make_unique<moon::MatchStmt>();
        value->scrutinee = lowerExpr(match->scrutinee.get());
        value->matchedType = typeRef(match->matchedType);
        for (const auto& sourceArm : match->arms) {
            moon::MatchArm arm;
            arm.location = locationOf(&sourceArm);
            arm.variantName = sourceArm.variantName;
            arm.variantIndex = static_cast<uint32_t>(sourceArm.variantIndex);
            arm.bindings = sourceArm.bindings;
            arm.bindingTypes = typeRefs(sourceArm.bindingTypes);
            arm.bindingUsages = sourceArm.bindingUsages;
            arm.body = lowerBlock(sourceArm.body.get());
            value->arms.push_back(std::move(arm));
        }
        result = std::move(value);
    } else if (auto* loop = dynamic_cast<const ::WhileStmt*>(statement)) {
        auto value = std::make_unique<moon::WhileStmt>();
        value->cond = lowerExpr(loop->cond.get());
        value->body = lowerBlock(loop->body.get());
        result = std::move(value);
    } else if (auto* loop = dynamic_cast<const ::ForStmt*>(statement)) {
        if (loop->isCompileTimeDeclarationViewLoop) {
            auto expanded = std::make_unique<moon::BlockStmt>();
            if (loop->compileTimeDeclarationIds.size() !=
                    loop->compileTimeSymbolNames.size() ||
                loop->compileTimeDeclarationIds.size() !=
                    loop->compileTimeSymbolIds.size() ||
                loop->compileTimeDeclarationIds.size() !=
                    loop->compileTimeContractIds.size()) {
                error(loop, "compile-time declaration_view loop has "
                      "inconsistent identity vectors");
                return nullptr;
            }
            const auto previous =
                mCompileTimeDeclarationBindings.find(loop->varName);
            const bool hadPrevious =
                previous != mCompileTimeDeclarationBindings.end();
            CompileTimeDeclarationBinding saved;
            if (hadPrevious) saved = previous->second;
            for (size_t index = 0;
                 index < loop->compileTimeDeclarationIds.size(); ++index) {
                mCompileTimeDeclarationBindings[loop->varName] = {
                    loop->compileTimeDeclarationIds[index],
                    loop->compileTimeSymbolNames[index],
                    loop->compileTimeSymbolIds[index],
                    loop->compileTimeContractIds[index],
                    loop->elementType && loop->elementType->inner
                        ? loop->elementType->inner : TyUnknown};
                auto body = lowerBlock(loop->body.get());
                if (body) expanded->stmts.push_back(std::move(body));
            }
            if (hadPrevious)
                mCompileTimeDeclarationBindings[loop->varName] =
                    std::move(saved);
            else
                mCompileTimeDeclarationBindings.erase(loop->varName);
            result = std::move(expanded);
        } else {
        auto value = std::make_unique<moon::ForStmt>();
        // Canonical compiler-recipe loops synthesize an i32/usize cursor and
        // bool branch condition after declaration references are resolved.
        // Freeze those pay-for-use compiler types before the type table seals.
        if (loop->protocolNextSymbol.empty()) {
            (void)typeRef(TyI32);
            (void)typeRef(TyUSize);
            (void)typeRef(TyBool);
        }
        value->varName = loop->varName;
        value->bindingUsage = loop->bindingUsage;
        value->iterable = lowerExpr(loop->iterable.get());
        value->body = lowerBlock(loop->body.get());
        value->elementType = typeRef(loop->elementType);
        deferDeclarationRef(value->protocolNext, loop->protocolNextSymbol,
                            loop, "Iterator::next protocol witness");
        value->protocolIteratorType = typeRef(loop->protocolIteratorType);
        value->protocolOptionType = typeRef(loop->protocolOptionType);
        value->protocolNoneVariant =
            static_cast<uint32_t>(loop->protocolNoneVariant);
        value->protocolSomeVariant =
            static_cast<uint32_t>(loop->protocolSomeVariant);
        deferDeclarationRef(value->protocolInto, loop->protocolIntoSymbol,
                            loop, "IntoIterator protocol witness");
        value->protocolInputType = typeRef(loop->protocolInputType);
        value->protocolStateName = loop->protocolStateName;
        value->protocolStateNeedsCleanup =
            loop->protocolStateNeedsCleanup;
        value->protocolStateCleanup =
            loop->protocolStateCleanup;
        value->recipeStateName = loop->recipeStateName;
        value->recipeSourceType = typeRef(loop->recipeSourceType);
        result = std::move(value);
        }
    } else if (auto* release = dynamic_cast<const ::FreeStmt*>(statement)) {
        auto value = std::make_unique<moon::FreeStmt>();
        value->operand = lowerExpr(release->operand.get());
        value->action = release->action;
        value->isImplicit = release->isImplicit;
        result = std::move(value);
    } else if (auto* slot = dynamic_cast<const ::SlotDeclStmt*>(statement)) {
        auto value = std::make_unique<moon::SlotDeclStmt>();
        value->name = slot->name;
        value->acceptedKind = slot->acceptedKind == ::FragmentKind::Interceptor
            ? moon::FragmentKind::Interceptor : moon::FragmentKind::Context;
        value->acceptedCardinality = slot->acceptedCardinality == ::FragmentCardinality::Once
            ? moon::FragmentCardinality::Once : moon::FragmentCardinality::Many;
        for (const auto& parameter : slot->params)
            value->params.push_back(lowerParam(parameter));
        value->defaultFragment = slot->defaultFragment;
        deferDeclarationRef(
            value->defaultFragmentRef,
            slot->resolvedDefaultFragmentName,
            slot, "slot default fragment");
        value->structuralType = typeRef(slot->structuralType);
        result = std::move(value);
    } else if (auto* slot = dynamic_cast<const ::SlotInvokeStmt*>(statement)) {
        auto value = std::make_unique<moon::SlotInvokeStmt>();
        value->name = slot->name;
        value->acceptedKind = slot->acceptedKind == ::FragmentKind::Interceptor
            ? moon::FragmentKind::Interceptor : moon::FragmentKind::Context;
        value->acceptedCardinality = slot->acceptedCardinality == ::FragmentCardinality::Once
            ? moon::FragmentCardinality::Once : moon::FragmentCardinality::Many;
        for (const auto& argument : slot->args)
            value->args.push_back(lowerExpr(argument.get()));
        value->continuation = lowerBlock(slot->continuation.get());
        value->isImplicitCapture = slot->isImplicitCapture;
        for (const auto& parameter : slot->interfaceParams)
            value->interfaceParams.push_back(lowerParam(parameter));
        value->resolvedParamNames = slot->resolvedParamNames;
        value->defaultFragment = slot->defaultFragment;
        deferDeclarationRef(
            value->defaultFragmentRef,
            slot->resolvedDefaultFragmentName,
            slot, "slot invocation default fragment");
        value->structuralType = typeRef(slot->structuralType);
        result = std::move(value);
    } else if (dynamic_cast<const ::ResumeStmt*>(statement)) {
        result = std::make_unique<moon::ResumeStmt>();
    } else if (auto* abort = dynamic_cast<const ::AbortStmt*>(statement)) {
        auto value = std::make_unique<moon::AbortStmt>();
        value->autoFrees = abort->autoFrees;
        for (const auto& cleanup : abort->cleanups) {
            moon::CleanupObligation lowered;
            lowered.place = cleanup.place;
            lowered.action = cleanup.action;
            if (cleanup.type) {
                if (mModule) mModule->registerType(cleanup.type);
                lowered.typeId = luna::types::typeId(cleanup.type);
            }
            value->cleanups.push_back(std::move(lowered));
        }
        result = std::move(value);
    } else if (auto* await = dynamic_cast<const ::AwaitStmt*>(statement)) {
        auto value = std::make_unique<moon::AwaitStmt>();
        value->event = lowerExpr(await->event.get());
        result = std::move(value);
    } else if (auto* apply = dynamic_cast<const ::ApplyStmt*>(statement)) {
        auto value = std::make_unique<moon::ApplyStmt>();
        value->slotName = apply->slotName;
        value->fragmentName = apply->fragmentName;
        deferDeclarationRef(
        value->fragmentRef,
            apply->resolvedFragmentName,
            apply, "apply fragment");
        value->body = lowerBlock(apply->body.get());
        result = std::move(value);
    } else {
        error(statement, "unsupported statement reached MoonIR lowering");
        return nullptr;
    }
    result->location = locationOf(statement);
    return result;
}

} // namespace moon
