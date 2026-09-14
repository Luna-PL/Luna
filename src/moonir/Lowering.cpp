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

std::unique_ptr<Module> LunaLowerer::lower(const Program& program,
                                           const SymbolTable& symbols,
                                           bool reserveKernelRuntime) {
    mErrors.clear();
    mProgram = &program;
    mSymbols = &symbols;
    mReserveKernelRuntime = reserveKernelRuntime;
    mRequiredKernelSymbols.clear();
    mPendingDeclarationRefs.clear();
    mCompileTimeDeclarationBindings.clear();
    mBuiltinTypeRefs.clear();
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
    buildModuleInterfaces();
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
    // Usage wrappers are binding contracts, not distinct runtime types. Keep
    // the resolved nominal identity of their inner type instead of sending the
    // wrapper through the context-free fallback resolver.
    if (const auto* linear = dynamic_cast<const LinearTypeAST*>(type))
        return lowerType(linear->inner.get());
    if (const auto* affine = dynamic_cast<const AffineTypeAST*>(type))
        return lowerType(affine->inner.get());
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
    if (!mModule) return type ? luna::types::typeId(type) : TypeRef{};
    if (!type) return {};
    if (type->identityMode == luna::types::IdentityMode::Builtin) {
        const auto cached = mBuiltinTypeRefs.find(type.get());
        if (cached != mBuiltinTypeRefs.end()) return cached->second;
        const TypeRef registered = mModule->registerType(type);
        mBuiltinTypeRefs.emplace(type.get(), registered);
        return registered;
    }
    return mModule->registerType(type);
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

void LunaLowerer::error(const ASTNode* node, const std::string& message) {
    const auto location = locationOf(node);
    mErrors.push_back(diagnostic::format(
        "moon-lower", message, location.path, location.line, location.column,
        "all checked Luna constructs must lower to a target-independent MoonIR operation"));
}

} // namespace moon
