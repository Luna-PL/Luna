#include "Lowering.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"
#include "../lexer/Token.h"
#include "../parser/AST.h"
#include "../sema/SymbolTable.h"

#include <algorithm>

namespace moon {

namespace {

std::string declarationName(const ::Decl* declaration) {
    if (auto* function = dynamic_cast<const ::FunctionDecl*>(declaration)) return function->name;
    if (auto* fragment = dynamic_cast<const ::FragmentDecl*>(declaration)) return fragment->name;
    if (auto* structure = dynamic_cast<const ::StructDecl*>(declaration)) return structure->name;
    if (auto* enumeration = dynamic_cast<const ::EnumDecl*>(declaration)) return enumeration->name;
    if (auto* trait = dynamic_cast<const ::TraitDecl*>(declaration)) return trait->name;
    return "implementation";
}

std::string declarationSymbol(const ::Decl* declaration) {
    const auto name = declarationName(declaration);
    return declaration && !declaration->generatedSymbolName.empty()
        ? declaration->generatedSymbolName : name;
}

std::string declarationNamespace(const ::Decl* declaration,
                                 const std::string& fallbackPackage) {
    std::string result = declaration && !declaration->packageId.empty()
        ? declaration->packageId : fallbackPackage;
    if (declaration && !declaration->modulePath.empty())
        result += "::" + declaration->modulePath;
    return result;
}

std::string declarationIdentity(const ::Decl* declaration,
                                const std::string& fallbackPackage,
                                const std::string& kind,
                                const std::string& symbol) {
    return declarationNamespace(declaration, fallbackPackage) +
           "::" + kind + "::" + symbol;
}

} // namespace

std::unique_ptr<Module> LunaLowerer::lower(const Program& program,
                                           const SymbolTable& symbols,
                                           bool reserveKernelRuntime) {
    mErrors.clear();
    mProgram = &program;
    mSymbols = &symbols;
    mReserveKernelRuntime = reserveKernelRuntime;
    mRequiredKernelSymbols.clear();
    mPendingDeclarationRefs.clear();
    auto module = std::make_unique<Module>();
    mModule = module.get();
    module->name = program.packageName.empty() ? "main" : program.packageName;
    module->isPackage = program.isPackage;
    module->sourceFiles = program.sourceFiles;
    for (const auto& use : program.packageUses)
        module->packageUses.push_back({
            use.ownerPackageId.empty() ? module->name : use.ownerPackageId,
            use.packageId, use.alias});
    module->sourceModules = program.sourceModules;
    module->features.kernelRuntimeReserved = reserveKernelRuntime;

    for (const auto& declaration : program.declarations) {
        if (!declaration) {
            error(nullptr, "frontend produced a null declaration");
            continue;
        }
        auto lowered = lowerDecl(declaration.get());
        if (lowered) module->declarations.push_back(std::move(lowered));
    }

    // Compiler-owned intrinsic traits intentionally have no source
    // declaration. Canonical MoonIR still gives each referenced trait a
    // normal declaration-table row instead of retaining an ad-hoc name.
    const std::pair<const char*, const char*> intrinsicTraits[] = {
        {luna::sysmeta::DropTraitId, "Drop"},
        {luna::sysmeta::FromTraitId, "From"},
    };
    for (const auto& intrinsicTrait : intrinsicTraits) {
        const char* traitId = intrinsicTrait.first;
        const char* sourceName = intrinsicTrait.second;
        const bool needed = std::any_of(
            mPendingDeclarationRefs.begin(), mPendingDeclarationRefs.end(),
            [traitId](const PendingDeclarationRef& pending) {
                return pending.lookupById && pending.lookup == traitId;
            });
        const bool present = std::any_of(
            module->declarationTable.begin(), module->declarationTable.end(),
            [traitId](const DeclarationRecord& declaration) {
                return declaration.id == traitId;
            });
        if (!needed || present) continue;
        TypePtr traitType = Type::makeTrait(sourceName);
        traitType->nominalId = traitId;
        DeclarationRecord record;
        record.id = traitId;
        record.familyId = record.id;
        record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
        record.sourceName = sourceName;
        record.kind = DeclarationKind::Trait;
        record.type = typeRef(traitType);
        record.canonicalContract = canonicalContract(record);
        record.contractId = luna::identity::contractIdFromCanonical(
            record.canonicalContract);
        record.sysmeta.identity.symbol = record.symbolId;
        record.sysmeta.identity.contract = record.contractId;
        module->declarationTable.push_back(std::move(record));
    }

    for (auto& declaration : module->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (!function || !function->isKernel) continue;
        const std::string symbol = function->generatedSymbolName.empty()
            ? function->name : function->generatedSymbolName;
        function->isCodegenReachable = reserveKernelRuntime ||
            mRequiredKernelSymbols.erase(symbol) != 0;
        if (!function->isCodegenReachable) continue;
        module->features.kernel = true;
        module->costs.push_back({
            CostKind::KernelCode,
            function->declarationId,
            reserveKernelRuntime
                ? "retained by --reserve-kernel-runtime"
                : "referenced by a lowered launch operation",
            function->location,
        });
    }
    for (const auto& missing : mRequiredKernelSymbols)
        error(nullptr, "kernel launch references missing MoonIR declaration '" +
                       missing + "'");

    if (reserveKernelRuntime) {
        module->features.kernel = true;
        module->costs.push_back({
            CostKind::ReservedCapability,
            "kernel runtime",
            "requested by --reserve-kernel-runtime",
            {},
        });
    }
    module->sealTypeTable();
    resolveDeclarationReferences();
    mModule = nullptr;
    mSymbols = nullptr;
    mProgram = nullptr;
    mReserveKernelRuntime = false;
    mRequiredKernelSymbols.clear();
    mPendingDeclarationRefs.clear();
    return module;
}

SourceLocation LunaLowerer::locationOf(const ASTNode* node) const {
    if (!node) return {};
    return {node->sourcePath, node->line, node->col};
}

TypePtr LunaLowerer::lowerType(const TypeAST* type) const {
    if (!type) {
        if (mModule) mModule->registerType(TyUnit);
        return TyUnit;
    }
    TypePtr result;
    if (auto* named = dynamic_cast<const NamedTypeAST*>(type)) {
        if (named->resolvedType) result = named->resolvedType;
        if (mSymbols) {
            const auto identity = named->resolvedType && !named->resolvedType->nominalId.empty()
                ? named->resolvedType->nominalId : named->name;
            if (!result) result = mSymbols->lookupType(identity);
            if (!result) result = mSymbols->lookupType(named->name);
        }
    }
    if (!result) result = resolveType(type, {});
    if (mModule) mModule->registerType(result);
    return result;
}

TypeRef LunaLowerer::typeRef(const TypePtr& type) const {
    return mModule ? mModule->registerType(type)
                   : (type ? luna::types::typeId(type) : TypeRef{});
}

TypeRefVec LunaLowerer::typeRefs(const TypeVec& types) const {
    TypeRefVec result;
    result.reserve(types.size());
    for (const auto& type : types) result.push_back(typeRef(type));
    return result;
}

TypePtr LunaLowerer::parameterType(const ::Param& parameter) const {
    return parameter.inferredType ? parameter.inferredType : lowerType(parameter.type.get());
}

Param LunaLowerer::lowerParam(const ::Param& parameter) const {
    auto type = parameterType(parameter);
    return {parameter.name, parameter.isLinear, parameter.usage,
            parameter.relation, typeRef(type)};
}

Operator LunaLowerer::lowerOperator(int rawTokenKind, const ASTNode* node) {
    const auto token = static_cast<TokenKind>(rawTokenKind);
    switch (token) {
        case TokenKind::Plus: return Operator::Add;
        case TokenKind::Minus: return Operator::Subtract;
        case TokenKind::Star: return Operator::Multiply;
        case TokenKind::Slash: return Operator::Divide;
        case TokenKind::Percent: return Operator::Remainder;
        case TokenKind::Eq: return Operator::Assign;
        case TokenKind::PlusEq: return Operator::AddAssign;
        case TokenKind::MinusEq: return Operator::SubtractAssign;
        case TokenKind::StarEq: return Operator::MultiplyAssign;
        case TokenKind::SlashEq: return Operator::DivideAssign;
        case TokenKind::PercentEq: return Operator::RemainderAssign;
        case TokenKind::AndEq: return Operator::BitAndAssign;
        case TokenKind::OrEq: return Operator::BitOrAssign;
        case TokenKind::XorEq: return Operator::BitXorAssign;
        case TokenKind::ShiftLeftEq: return Operator::ShiftLeftAssign;
        case TokenKind::ShiftRightEq: return Operator::ShiftRightAssign;
        case TokenKind::EqEq: return Operator::Equal;
        case TokenKind::Neq: return Operator::NotEqual;
        case TokenKind::Lt: return Operator::Less;
        case TokenKind::LtEq: return Operator::LessEqual;
        case TokenKind::Gt: return Operator::Greater;
        case TokenKind::GtEq: return Operator::GreaterEqual;
        case TokenKind::ShiftLeft: return Operator::ShiftLeft;
        case TokenKind::ShiftRight: return Operator::ShiftRight;
        case TokenKind::AndAnd: return Operator::LogicalAnd;
        case TokenKind::OrOr: return Operator::LogicalOr;
        case TokenKind::Ampersand: return Operator::BitAnd;
        case TokenKind::BitOr: return Operator::BitOr;
        case TokenKind::BitXor: return Operator::BitXor;
        case TokenKind::Not: return Operator::LogicalNot;
        case TokenKind::Tilde: return Operator::BitNot;
        default:
            error(node, "source operator cannot be represented in MoonIR");
            return Operator::Add;
    }
}

std::unique_ptr<moon::Expr> LunaLowerer::lowerExpr(const ::Expr* expression) {
    if (!expression) return nullptr;
    std::unique_ptr<moon::Expr> result;
    if (auto* literal = dynamic_cast<const ::IntLiteralExpr*>(expression)) {
        auto value = std::make_unique<moon::IntLiteralExpr>();
        value->value = literal->value;
        value->type = typeRef(TyI32);
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
        value->name = identifier->name;
        deferDeclarationRef(
            value->declaration, identifier->resolvedSymbolName,
            identifier, "declaration-valued identifier");
        result = std::move(value);
    } else if (auto* binary = dynamic_cast<const ::BinaryExpr*>(expression)) {
        auto value = std::make_unique<moon::BinaryExpr>();
        value->lhs = lowerExpr(binary->lhs.get());
        value->op = lowerOperator(static_cast<int>(binary->op), binary);
        value->rhs = lowerExpr(binary->rhs.get());
        result = std::move(value);
    } else if (auto* unary = dynamic_cast<const ::UnaryExpr*>(expression)) {
        auto value = std::make_unique<moon::UnaryExpr>();
        if (unary->op == TokenKind::Minus) value->op = Operator::Negate;
        else if (unary->op == TokenKind::Star) value->op = Operator::Dereference;
        else value->op = lowerOperator(static_cast<int>(unary->op), unary);
        value->operand = lowerExpr(unary->operand.get());
        result = std::move(value);
    } else if (auto* call = dynamic_cast<const ::CallExpr*>(expression)) {
        auto value = std::make_unique<moon::CallExpr>();
        value->callee = lowerExpr(call->callee.get());
        for (const auto& argument : call->args)
            value->args.push_back(lowerExpr(argument.get()));
        value->typeArgs = typeRefs(call->typeArgs);
        deferDeclarationRef(value->calleeRef, call->resolvedSymbolName,
                            call, "call target");
        if (!call->resolvedSymbolName.empty()) {
            if (auto* callee = dynamic_cast<moon::IdentifierExpr*>(
                    value->callee.get()))
                deferDeclarationRef(
                    callee->declaration, call->resolvedSymbolName,
                    call, "resolved call identifier");
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
        result = std::move(value);
    } else if (auto* assignment = dynamic_cast<const ::AssignExpr*>(expression)) {
        auto value = std::make_unique<moon::AssignExpr>();
        value->op = lowerOperator(static_cast<int>(assignment->op), assignment);
        value->lhs = lowerExpr(assignment->lhs.get());
        value->rhs = lowerExpr(assignment->rhs.get());
        result = std::move(value);
    } else if (auto* selection = dynamic_cast<const ::SelectExpr*>(expression)) {
        if (selection->isDynamic) {
            auto value = std::make_unique<moon::DynamicSelectExpr>();
            value->familyId = selection->resolvedFamilyId;
            deferDeclarationRef(
                value->selector,
                selection->resolvedSelectorDeclarationId,
                selection, "dynamic selector", true);
            value->metadataSchemaId = selection->dynamicMetadataSchemaId;
            value->type = typeRef(selection->selectedType);
            for (const auto& binding : selection->dynamicFilterArguments) {
                if (binding.selectorArgumentIndex &&
                    *binding.selectorArgumentIndex < selection->selectorArgs.size()) {
                    value->filterArguments.push_back(lowerExpr(
                        selection->selectorArgs[*binding.selectorArgumentIndex].get()));
                    continue;
                }
                std::unique_ptr<moon::Expr> literal;
                if (binding.constant) {
                    if (auto* integer = std::get_if<int64_t>(&*binding.constant)) {
                        auto node = std::make_unique<moon::IntLiteralExpr>();
                        node->value = *integer;
                        node->type = typeRef(TyI32);
                        literal = std::move(node);
                    } else if (auto* floating = std::get_if<double>(&*binding.constant)) {
                        auto node = std::make_unique<moon::FloatLiteralExpr>();
                        node->value = *floating;
                        node->type = typeRef(TyF64);
                        literal = std::move(node);
                    } else if (auto* boolean = std::get_if<bool>(&*binding.constant)) {
                        auto node = std::make_unique<moon::BoolLiteralExpr>();
                        node->value = *boolean;
                        node->type = typeRef(TyBool);
                        literal = std::move(node);
                    } else if (auto* string = std::get_if<std::string>(&*binding.constant)) {
                        auto node = std::make_unique<moon::StringLiteralExpr>();
                        node->value = *string;
                        node->type = typeRef(TyString);
                        literal = std::move(node);
                    }
                }
                if (!literal) {
                    error(selection, "dynamic selector filter has no lowered value");
                    return nullptr;
                }
                literal->location = locationOf(selection);
                value->filterArguments.push_back(std::move(literal));
            }
            value->candidates.reserve(selection->dynamicCandidates.size());
            for (const auto& source : selection->dynamicCandidates) {
                DynamicSelectCandidate candidate;
                candidate.metadataValues = source.metadataValues;
                value->candidates.push_back(std::move(candidate));
            }
            for (size_t index = 0;
                 index < selection->dynamicCandidates.size(); ++index)
                deferDeclarationRef(
                    value->candidates[index].declaration,
                    selection->dynamicCandidates[index].declarationId,
                    selection, "dynamic select candidate", true);
            mModule->features.runtime = true;
            mModule->features.dynamicSelect = true;
            mModule->costs.push_back({CostKind::DynamicBinding,
                                      selection->targetName,
                                      "dynamic select operation",
                                      locationOf(selection)});
            result = std::move(value);
        } else {
            auto value = std::make_unique<moon::IdentifierExpr>();
            value->name = selection->resolvedSymbolName;
            deferDeclarationRef(
                value->declaration, selection->resolvedSymbolName,
                selection, "statically selected declaration");
            value->type = typeRef(selection->selectedType);
            result = std::move(value);
        }
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
            let->inferredType->kind == TypeKind::DeclarationRef)
            return nullptr;
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
        auto value = std::make_unique<moon::ForStmt>();
        // Canonical compiler-recipe loops synthesize an i32 cursor and bool
        // branch condition after declaration references are resolved. Freeze
        // those pay-for-use compiler types before the module type table seals.
        if (loop->protocolNextSymbol.empty()) {
            (void)typeRef(TyI32);
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
        value->isDynamic = slot->isDynamic;
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
        value->isDynamic = slot->isDynamic;
        value->usesDynamicDispatch = slot->usesDynamicDispatch;
        value->dynamicFragmentRefs.resize(
            slot->resolvedDynamicFragmentNames.size());
        for (size_t index = 0;
             index < slot->resolvedDynamicFragmentNames.size(); ++index)
            deferDeclarationRef(
                value->dynamicFragmentRefs[index],
                slot->resolvedDynamicFragmentNames[index],
                slot, "dynamic slot fragment candidate");
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
        if (slot->usesDynamicDispatch) {
            mModule->features.runtime = true;
            mModule->features.dynamicApply = true;
        }
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
        value->isDynamic = apply->isDynamic;
        value->alternativeFragmentNames = apply->alternativeFragmentNames;
        value->alternativeFragmentRefs.resize(
            apply->resolvedAlternativeFragmentNames.size());
        for (size_t index = 0;
             index < apply->resolvedAlternativeFragmentNames.size(); ++index)
            deferDeclarationRef(
                value->alternativeFragmentRefs[index],
                apply->resolvedAlternativeFragmentNames[index],
                apply, "dynamic apply fragment candidate");
        deferDeclarationRef(
            value->fragmentRef,
            apply->resolvedFragmentName,
            apply, "apply fragment");
        value->body = lowerBlock(apply->body.get());
        if (apply->isDynamic) {
            mModule->features.runtime = true;
            mModule->features.dynamicApply = true;
            mModule->costs.push_back({CostKind::DynamicBinding, apply->slotName,
                                      "dynamic apply operation", locationOf(apply)});
        }
        result = std::move(value);
    } else {
        error(statement, "unsupported statement reached MoonIR lowering");
        return nullptr;
    }
    result->location = locationOf(statement);
    return result;
}

std::unique_ptr<moon::BlockStmt> LunaLowerer::lowerBlock(const ::BlockStmt* block) {
    if (!block) return nullptr;
    auto result = std::make_unique<moon::BlockStmt>();
    result->location = locationOf(block);
    for (const auto& statement : block->stmts) {
        auto lowered = lowerStmt(statement.get());
        if (lowered) result->stmts.push_back(std::move(lowered));
    }
    return result;
}

std::unique_ptr<moon::FunctionDecl> LunaLowerer::lowerFunction(
    const ::FunctionDecl* function) {
    if (!function) return nullptr;
    auto result = std::make_unique<moon::FunctionDecl>();
    result->location = locationOf(function);
    result->name = function->name;
    result->generatedSymbolName = function->generatedSymbolName.empty()
        ? function->name : function->generatedSymbolName;
    result->familyId = declarationIdentity(function, mModule->name, "fn", function->name);
    result->declarationId = declarationIdentity(
        function, mModule->name, "fn", result->generatedSymbolName);
    result->isExported = function->isExported;
    result->isKernel = function->isKernel;
    result->isExtern = function->isExtern;
    result->isConstexpr = function->isConstexpr;
    result->isSelector = function->isSelector;
    result->abi = function->abi;
    result->linkName = function->linkName;
    result->typeParams = function->typeParams;
    TypeVec parameterTypes;
    for (const auto& parameter : function->params) {
        parameterTypes.push_back(parameterType(parameter));
        result->params.push_back(lowerParam(parameter));
    }
    const TypePtr returnType = function->inferredReturnType
        ? function->inferredReturnType
        : (function->returnType
            ? lowerType(function->returnType.get()) : TyUnit);
    result->returnType = typeRef(returnType);
    result->returnsLinear = function->returnsLinear;
    result->returnUsage = function->returnUsage;
    result->body = lowerBlock(function->body.get());
    result->isTemplateInstance = function->isTemplateInstance;
    result->concreteTypeArgs = typeRefs(function->concreteTypeArgs);
    lowerCommonDeclaration(function, *result);

    std::vector<luna::ownership::Contract> parameterContracts;
    for (const auto& parameter : result->params) {
        parameterContracts.push_back({parameter.relation, parameter.usage});
    }
    const auto callableType = Type::makeFunction(
        std::move(parameterTypes), returnType, std::move(parameterContracts),
        {luna::ownership::Relation::Owned, result->returnUsage});
    result->sysmeta = callableType->sysmeta;
    result->sysmeta.capability.ffi =
        result->isExtern || !result->abi.empty();
    result->sysmeta.capability.gpu = result->isKernel;
    result->sysmeta.capability.hostOnly = !result->isKernel;
    result->sysmeta.capability.runtimeRetained =
        function->retention != RetentionKind::CompileTime;
    result->sysmeta.abi.stableBoundary =
        result->isExtern || !result->abi.empty();
    addDeclarationRecord(*result, DeclarationKind::Function, callableType);
    if (!result->isKernel && !result->isSelector &&
               (result->typeParams.empty() || result->isTemplateInstance)) {
        mModule->costs.push_back({CostKind::MachineCode, result->declarationId,
                                  result->isTemplateInstance
                                      ? "concrete generic instance" : "concrete function",
                                  result->location});
    }
    if (result->isTemplateInstance) {
        mModule->costs.push_back({CostKind::GenericInstantiation, result->declarationId,
                                  "requested by a statically resolved call",
                                  result->location});
    }
    return result;
}

std::unique_ptr<moon::Decl> LunaLowerer::lowerDecl(const ::Decl* declaration) {
    if (auto* function = dynamic_cast<const ::FunctionDecl*>(declaration)) {
        // A purely static selector has already reduced every use to a concrete
        // declaration. Erase its compiler-only function and view types before
        // MoonIR so the zero-cost rule is structural, not merely a backend
        // optimization.
        if (function->isSelector && !function->isDynamicSelector)
            return nullptr;
        return lowerFunction(function);
    }
    if (auto* fragment = dynamic_cast<const ::FragmentDecl*>(declaration)) {
        auto result = std::make_unique<moon::FragmentDecl>();
        result->location = locationOf(fragment);
        result->name = fragment->name;
        result->generatedSymbolName = declarationSymbol(fragment);
        result->familyId = declarationIdentity(fragment, mModule->name, "fragment", fragment->name);
        result->declarationId = declarationIdentity(
            fragment, mModule->name, "fragment", result->generatedSymbolName);
        result->isExported = fragment->isExported;
        result->kind = fragment->kind == ::FragmentKind::Interceptor
            ? moon::FragmentKind::Interceptor : moon::FragmentKind::Context;
        result->cardinality = fragment->cardinality == ::FragmentCardinality::Once
            ? moon::FragmentCardinality::Once : moon::FragmentCardinality::Many;
        for (const auto& parameter : fragment->params)
            result->params.push_back(lowerParam(parameter));
        result->body = lowerBlock(fragment->body.get());
        const TypePtr structuralType = fragment->structuralType;
        result->structuralType = typeRef(structuralType);
        if (structuralType)
            result->sysmeta = structuralType->sysmeta;
        lowerCommonDeclaration(fragment, *result);
        addDeclarationRecord(*result, DeclarationKind::Fragment, structuralType);
        return result;
    }
    if (auto* structure = dynamic_cast<const ::StructDecl*>(declaration)) {
        auto result = std::make_unique<moon::StructDecl>();
        result->location = locationOf(structure);
        result->name = structure->name;
        result->generatedSymbolName = declarationSymbol(structure);
        result->familyId = declarationIdentity(structure, mModule->name, "struct", structure->name);
        result->declarationId = declarationIdentity(
            structure, mModule->name, "struct", result->generatedSymbolName);
        result->isExported = structure->isExported;
        result->typeParams = structure->typeParams;
        for (const auto& field : structure->fields)
            result->fields.push_back(lowerParam(field));
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(result->generatedSymbolName) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(structure->name);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(structure, *result);
        addDeclarationRecord(*result, DeclarationKind::Struct, declarationType);
        return result;
    }
    if (auto* enumeration = dynamic_cast<const ::EnumDecl*>(declaration)) {
        auto result = std::make_unique<moon::EnumDecl>();
        result->location = locationOf(enumeration);
        result->name = enumeration->name;
        result->generatedSymbolName = declarationSymbol(enumeration);
        result->familyId = declarationIdentity(enumeration, mModule->name, "enum", enumeration->name);
        result->declarationId = declarationIdentity(
            enumeration, mModule->name, "enum", result->generatedSymbolName);
        result->isExported = enumeration->isExported;
        result->typeParams = enumeration->typeParams;
        for (const auto& variant : enumeration->variants) {
            moon::EnumDecl::Variant lowered;
            lowered.name = variant.name;
            for (const auto& field : variant.fields)
                lowered.fields.push_back(typeRef(lowerType(field.get())));
            result->variants.push_back(std::move(lowered));
        }
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(result->generatedSymbolName) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(enumeration->name);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(enumeration, *result);
        addDeclarationRecord(*result, DeclarationKind::Enum, declarationType);
        return result;
    }
    if (auto* trait = dynamic_cast<const ::TraitDecl*>(declaration)) {
        auto result = std::make_unique<moon::TraitDecl>();
        result->location = locationOf(trait);
        result->name = trait->name;
        result->generatedSymbolName = declarationSymbol(trait);
        result->familyId = declarationIdentity(trait, mModule->name, "trait", trait->name);
        result->declarationId = declarationIdentity(
            trait, mModule->name, "trait", result->generatedSymbolName);
        result->isExported = trait->isExported;
        result->typeParams = trait->typeParams;
        for (const auto& parameter : trait->traitParams)
            result->traitParams.push_back(lowerParam(parameter));
        for (const auto& method : trait->methods) {
            moon::TraitDecl::MethodSig lowered;
            lowered.name = method.name;
            for (const auto& parameter : method.params)
                lowered.params.push_back(lowerParam(parameter));
            lowered.returnType = typeRef(lowerType(method.returnType.get()));
            result->methods.push_back(std::move(lowered));
        }
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(trait->resolvedTraitId) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(result->generatedSymbolName);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(trait, *result);
        addDeclarationRecord(*result, DeclarationKind::Trait, declarationType);
        return result;
    }
    if (auto* implementation = dynamic_cast<const ::ImplDecl*>(declaration)) {
        auto result = std::make_unique<moon::ImplDecl>();
        result->location = locationOf(implementation);
        result->name = "impl";
        const std::string resolvedTraitId =
            implementation->trait.resolvedTraitId;
        const std::string resolvedTargetTypeId =
            implementation->resolvedTargetTypeId;
        result->generatedSymbolName = implementation->generatedSymbolName.empty()
            ? resolvedTraitId + "__for__" + resolvedTargetTypeId
            : implementation->generatedSymbolName;
        result->familyId = declarationIdentity(
            implementation, mModule->name, "impl", resolvedTraitId);
        result->declarationId = declarationIdentity(
            implementation, mModule->name, "impl", result->generatedSymbolName);
        result->typeParams = implementation->typeParams;
        deferDeclarationRef(
            result->traitRef, resolvedTraitId,
            implementation, "implementation trait", true);
        const TypePtr targetType = lowerType(implementation->targetType.get());
        result->targetType = typeRef(targetType);
        for (const auto& method : implementation->methods)
            result->methods.push_back(lowerFunction(method.get()));
        lowerCommonDeclaration(implementation, *result);
        addDeclarationRecord(*result, DeclarationKind::Implementation, targetType);
        return result;
    }
    if (auto* metadata = dynamic_cast<const ::MetaDecl*>(declaration)) {
        MetadataSchema schema;
        const auto schemaSymbol = metadata->generatedSymbolName.empty()
            ? metadata->name : metadata->generatedSymbolName;
        schema.id = declarationIdentity(metadata, mModule->name, "meta", schemaSymbol);
        schema.name = metadata->name;
        schema.location = locationOf(metadata);
        for (const auto& field : metadata->fields)
            schema.fields.push_back({field.name, typeRef(field.inferredType)});
        mModule->metadataSchemas.push_back(std::move(schema));

        DeclarationRecord record;
        record.id = declarationIdentity(metadata, mModule->name, "meta",
            metadata->generatedSymbolName.empty()
                ? metadata->name : metadata->generatedSymbolName);
        record.familyId = declarationIdentity(
            metadata, mModule->name, "meta", metadata->name);
        record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
        record.sourceName = metadata->name;
        record.linkageName = metadata->generatedSymbolName;
        record.kind = DeclarationKind::MetadataSchema;
        record.retention = lowerRetention(metadata->retention);
        record.sysmeta.capability.runtimeRetained =
            metadata->retention != RetentionKind::CompileTime;
        record.location = locationOf(metadata);
        const TypePtr metadataType = mSymbols
            ? mSymbols->lookupType(schemaSymbol) : nullptr;
        record.type = typeRef(metadataType);
        record.canonicalContract = moon::canonicalContract(record);
        record.contractId = luna::identity::contractIdFromCanonical(
            record.canonicalContract);
        record.sysmeta.identity.symbol = record.symbolId;
        record.sysmeta.identity.contract = record.contractId;
        mModule->declarationTable.push_back(std::move(record));
        return nullptr;
    }
    // Named constraints are fully discharged by semantic analysis. Keeping
    // them out of MoonIR preserves the default zero-runtime-cost boundary.
    if (dynamic_cast<const ::ConstraintDecl*>(declaration))
        return nullptr;
    error(declaration, "unsupported declaration reached MoonIR lowering");
    return nullptr;
}

Retention LunaLowerer::lowerRetention(RetentionKind retention) const {
    if (retention == RetentionKind::Runtime) return Retention::Runtime;
    if (retention == RetentionKind::Dynamic) return Retention::Dynamic;
    return Retention::CompileTime;
}

void LunaLowerer::lowerCommonDeclaration(const ::Decl* source,
                                         moon::Decl& target) {
    target.symbolId = luna::identity::symbolIdFromCanonical(
        target.declarationId);
    target.packageId = source->packageId.empty() ? mModule->name : source->packageId;
    target.modulePath = source->modulePath;
    target.retention = lowerRetention(source->retention);
    target.sysmeta.capability.runtimeRetained =
        source->retention != RetentionKind::CompileTime;
    for (const auto& attachment : source->metadata) {
        MetadataInstance instance;
        instance.schemaId = attachment.resolvedSchemaId;
        instance.values = attachment.evaluatedArguments;
        instance.retention = lowerRetention(attachment.retention);
        instance.location = locationOf(source);
        target.metadata.push_back(std::move(instance));
        if (attachment.retention != RetentionKind::CompileTime) {
            mModule->features.runtime = true;
            mModule->costs.push_back({CostKind::RuntimeMetadata,
                                      target.declarationId + "@" + attachment.schemaName,
                                      "explicit metadata retention",
                                      target.location});
        }
    }
    if (source->retention != RetentionKind::CompileTime) {
        mModule->features.runtime = true;
        if (source->retention == RetentionKind::Dynamic)
            mModule->features.dynamicReflection = true;
        mModule->costs.push_back({CostKind::RuntimeDescriptor,
                                  target.declarationId,
                                  source->retention == RetentionKind::Dynamic
                                      ? "dynamic declaration" : "runtime declaration",
                                  target.location});
    }
}

TypePtr LunaLowerer::inferredExprType(const ::Expr* expression) const {
    if (!expression) return nullptr;
    if (dynamic_cast<const ::IntLiteralExpr*>(expression)) return TyI32;
    if (dynamic_cast<const ::FloatLiteralExpr*>(expression)) return TyF64;
    if (dynamic_cast<const ::StringLiteralExpr*>(expression)) return TyString;
    if (dynamic_cast<const ::BoolLiteralExpr*>(expression)) return TyBool;
    if (auto* variant = dynamic_cast<const ::VariantConstructExpr*>(expression))
        return variant->constructedType;
    if (auto* array = dynamic_cast<const ::ArrayLiteralExpr*>(expression))
        return Type::makeArray(array->elementType, array->elements.size());
    if (auto* record = dynamic_cast<const ::RecordLiteralExpr*>(expression))
        return record->recordType;
    if (auto* allocation = dynamic_cast<const ::HeapAllocExpr*>(expression))
        return allocation->resultType
            ? allocation->resultType
            : Type::makeRawPointer(allocation->allocatedType);
    if (dynamic_cast<const ::LaunchExpr*>(expression)) return TyEvent;
    return nullptr;
}

void LunaLowerer::addDeclarationRecord(const moon::Decl& declaration,
                                       DeclarationKind kind, TypePtr type) {
    DeclarationRecord record;
    record.id = declaration.declarationId;
    record.familyId = declaration.familyId;
    record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
    record.sourceName = declaration.name;
    record.linkageName = declaration.generatedSymbolName;
    record.kind = kind;
    record.retention = declaration.retention;
    record.metadata = declaration.metadata;
    record.type = typeRef(type);
    record.sysmeta = declaration.sysmeta;
    if (type) {
        record.sysmeta.resource = type->sysmeta.resource;
        if (type->kind == TypeKind::Slot ||
            type->kind == TypeKind::Fragment)
            record.sysmeta.control = type->sysmeta.control;
    }
    record.canonicalContract = moon::canonicalContract(record);
    record.contractId = luna::identity::contractIdFromCanonical(
        record.canonicalContract);
    record.sysmeta.identity.symbol = record.symbolId;
    record.sysmeta.identity.contract = record.contractId;
    record.location = declaration.location;
    mModule->declarationTable.push_back(std::move(record));
}

void LunaLowerer::deferDeclarationRef(
    DeclarationRef& target,
    std::string lookup,
    const ASTNode* source,
    std::string context,
    bool lookupById) {
    if (lookup.empty()) return;
    mPendingDeclarationRefs.push_back({
        &target, std::move(lookup), source, std::move(context), lookupById});
}

void LunaLowerer::resolveDeclarationReferences() {
    if (!mModule) return;
    mModule->rebuildIndexes();

    // Drop glue belongs to the frozen type/declaration contract, not to an
    // opaque backend linkage string. Resolve it first because declaration
    // ContractIds include the expected drop implementation.
    for (auto& type : mModule->typeTable) {
        const std::string linkage = type.sysmeta.abi.dropGlueSymbol;
        type.sysmeta.abi.dropGlueSymbol.clear();
        if (linkage.empty()) continue;
        const auto* declaration =
            mModule->findDeclarationByLinkage(linkage);
        if (!declaration) {
            error(nullptr, "type '" + type.displayName +
                           "' references missing Drop glue declaration '" +
                           linkage + "'");
            continue;
        }
        type.dropGlue = {declaration->symbolId, declaration->contractId};
    }

    for (auto& declaration : mModule->declarationTable) {
        if (const auto* type = mModule->findType(declaration.type))
            declaration.dropGlue = type->dropGlue;
        declaration.sysmeta.abi.dropGlueSymbol.clear();
        declaration.canonicalContract = canonicalContract(declaration);
        declaration.contractId = luna::identity::contractIdFromCanonical(
            declaration.canonicalContract);
        declaration.sysmeta.identity.contract = declaration.contractId;
    }
    mModule->rebuildIndexes();

    for (const auto& pending : mPendingDeclarationRefs) {
        if (!pending.target) continue;
        const auto* declaration = pending.lookupById
            ? mModule->findDeclarationById(pending.lookup)
            : mModule->findDeclarationByLinkage(pending.lookup);
        if (!declaration) {
            error(pending.source, pending.context +
                  " references missing MoonIR declaration '" +
                  pending.lookup + "'");
            continue;
        }
        *pending.target = {
            declaration->symbolId, declaration->contractId};
    }

    const auto finalizeExecutable = [&](auto&& self, moon::Decl& declaration)
        -> void {
        const auto* record = mModule->findDeclaration(declaration.symbolId);
        if (record) declaration.contractId = record->contractId;
        declaration.sysmeta.abi.dropGlueSymbol.clear();
        if (auto* implementation =
                dynamic_cast<moon::ImplDecl*>(&declaration))
            for (auto& method : implementation->methods)
                if (method) self(self, *method);
    };
    for (auto& declaration : mModule->declarations)
        if (declaration) finalizeExecutable(
            finalizeExecutable, *declaration);
}

void LunaLowerer::error(const ASTNode* node, const std::string& message) {
    const auto location = locationOf(node);
    mErrors.push_back(diagnostic::format(
        "moon-lower", message, location.path, location.line, location.column,
        "all checked Luna constructs must lower to a target-independent MoonIR operation"));
}

} // namespace moon
