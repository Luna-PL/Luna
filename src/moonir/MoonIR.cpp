#include "MoonIR.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <stdexcept>

namespace moon {

namespace {

void appendIdentityPart(std::string& output, const std::string& part) {
    output += std::to_string(part.size());
    output += ':';
    output += part;
    output += ';';
}

template <typename Enum>
void appendEnum(std::string& output, Enum value) {
    appendIdentityPart(
        output, std::to_string(static_cast<unsigned>(value)));
}

void appendBool(std::string& output, bool value) {
    appendIdentityPart(output, value ? "1" : "0");
}

void appendOwnershipContract(
    std::string& output, const luna::ownership::Contract& contract) {
    appendEnum(output, contract.relation);
    appendEnum(output, contract.usage);
}

} // namespace

LambdaExpr::~LambdaExpr() = default;

std::string canonicalAbiLayout(const TypeRecord& type) {
    std::string result = "luna.abi-layout.v1;";
    appendIdentityPart(result, std::to_string(type.layoutAbiVersion));
    appendIdentityPart(result, std::to_string(type.valueSize));
    appendIdentityPart(result, std::to_string(type.valueAlignment));
    appendIdentityPart(result, type.abiLayout);
    return result;
}

std::string canonicalContract(const DeclarationRecord& declaration) {
    std::string result = "luna.contract.v1;";
    appendEnum(result, declaration.kind);
    appendIdentityPart(result, declaration.type.value);

    const auto& facts = declaration.sysmeta;
    appendEnum(result, facts.control.form);
    appendEnum(result, facts.control.cardinality);
    appendEnum(result, facts.control.storage);
    appendEnum(result, facts.control.forwarding);
    appendBool(result, facts.control.abortPermitted);
    appendBool(result, facts.control.replayValidated);

    appendIdentityPart(
        result, std::to_string(facts.resource.parameters.size()));
    for (const auto& parameter : facts.resource.parameters)
        appendOwnershipContract(result, parameter);
    appendOwnershipContract(result, facts.resource.result);
    appendEnum(result, facts.resource.management);
    appendEnum(result, facts.resource.releaseDomain);
    appendEnum(result, facts.resource.lifetime);
    appendEnum(result, facts.resource.relation);
    appendEnum(result, facts.resource.usage);
    appendEnum(result, facts.resource.cleanup);
    appendBool(result, facts.resource.cleanupRequired);
    appendBool(result, facts.resource.recursiveCleanup);
    appendBool(result, facts.resource.needsDrop);
    appendBool(result, facts.resource.tracksElementInitialization);

    appendBool(result, facts.capability.hostOnly);
    appendBool(result, facts.capability.runtimeRetained);
    appendBool(result, facts.capability.dynamicDispatch);
    appendBool(result, facts.capability.ffi);
    appendBool(result, facts.capability.gpu);
    appendBool(result, facts.capability.maySuspend);
    appendBool(result, facts.abi.stableBoundary);
    appendBool(result, facts.abi.persistentFrameRequired);
    appendIdentityPart(result, declaration.dropGlue.symbol.value);
    appendIdentityPart(result, declaration.dropGlue.contract.value);
    return result;
}

void Module::rebuildIndexes() {
    typesById.clear();
    for (size_t index = 0; index < typeTable.size(); ++index)
        typesById[typeTable[index].id.value] = index;
    declarationRecordsById.clear();
    declarationRecordsBySymbol.clear();
    declarationRecordsByLinkage.clear();
    for (size_t index = 0; index < declarationTable.size(); ++index) {
        const auto& declaration = declarationTable[index];
        declarationRecordsById[declaration.id] = index;
        declarationRecordsBySymbol[declaration.symbolId.value] = index;
        if (!declaration.linkageName.empty())
            declarationRecordsByLinkage[declaration.linkageName] = index;
    }
    declarationsById.clear();
    functionsBySymbol.clear();
    fragmentsBySymbol.clear();
    for (auto& declaration : declarations) {
        if (!declaration) continue;
        declarationsById[declaration->declarationId] = declaration.get();
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get())) {
            const auto& symbol = function->generatedSymbolName.empty()
                ? function->name : function->generatedSymbolName;
            functionsBySymbol[symbol] = function;
        } else if (auto* fragment = dynamic_cast<FragmentDecl*>(declaration.get())) {
            const auto& symbol = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            fragmentsBySymbol[symbol] = fragment;
        } else if (auto* implementation = dynamic_cast<ImplDecl*>(declaration.get())) {
            for (auto& method : implementation->methods) {
                if (!method) continue;
                const auto& symbol = method->generatedSymbolName.empty()
                    ? method->name : method->generatedSymbolName;
                functionsBySymbol[symbol] = method.get();
            }
        }
    }
}

TypeRef Module::registerType(const TypePtr& type) {
    if (!type) return {};
    if (typeTableSealed)
        throw std::logic_error("cannot register a type after the MoonIR type table is sealed");
    const auto canonicalTypeValue = luna::types::canonicalType(type);
    const auto id = luna::types::typeId(type);

    TypeRecord record;
    record.id = id;
    record.shapeId = luna::types::shapeId(type);
    record.domain = type->domain;
    record.identityMode = type->identityMode;
    record.kind = type->kind;
    record.sysmeta = type->sysmeta;
    const auto resource = resourceContractForType(type);
    record.sysmeta.resource.usage = resource.usage;
    record.sysmeta.resource.cleanup = resource.cleanup;
    record.sysmeta.resource.cleanupRequired = resource.cleanupRequired;
    record.sysmeta.resource.recursiveCleanup = resource.recursiveCleanup;
    record.sysmeta.resource.lifetime = resource.lifetime;
    record.sysmeta.resource.relation = resource.relation;
    record.displayName = type->toString();
    record.sourceName = type->name;
    record.declarationLinkageName = type->declarationLinkageName;
    record.nominalDeclarationId = type->nominalId;
    record.typeParameterNames = type->typeParams;
    record.arrayLength = type->arrayLength;
    record.isMutable = type->isMutable;
    record.parameterContracts = type->paramContracts;
    record.returnContract = type->returnContract;
    record.isMultiShot = type->isMultiShot;
    record.continuationKind = type->continuationKind;
    record.iteratorMode = type->iteratorMode;
    record.inferenceId = type->inferenceId;
    record.canonicalType = canonicalTypeValue;
    record.canonicalShape = luna::types::canonicalShape(type);
    record.valueSize = luna::layout::valueSize(type);
    record.valueAlignment = luna::layout::valueAlignment(type);
    if (type->kind == TypeKind::Enum ||
        type->kind == TypeKind::Result) {
        record.layoutAbiVersion =
            luna::layout::InlineAdtAbiVersion;
        record.abiLayout =
            luna::layout::inlineAdtLayoutSignature(type);
    }
    record.canonicalAbiLayout = canonicalAbiLayout(record);
    record.abiLayoutId = luna::identity::abiLayoutIdFromCanonical(
        record.canonicalAbiLayout);
    record.sysmeta.identity.type = record.id;
    record.sysmeta.identity.shape = record.shapeId;
    record.sysmeta.identity.abiLayout = record.abiLayoutId;
    const auto addReference = [&](const TypePtr& referenced) {
        if (referenced) record.referencedTypeIds.push_back(luna::types::typeId(referenced));
    };
    const auto referenceOf = [&](const TypePtr& referenced) {
        return referenced ? luna::types::typeId(referenced) : TypeRef{};
    };
    record.innerTypeId = referenceOf(type->inner);
    record.returnTypeId = referenceOf(type->returnType);
    for (const auto& argument : type->typeArgs)
        record.typeArgumentIds.push_back(referenceOf(argument));
    for (const auto& parameter : type->paramTypes)
        record.parameterTypeIds.push_back(referenceOf(parameter));
    for (const auto& field : type->fields)
        record.fields.push_back({field.name, referenceOf(field.type)});
    for (const auto& field : type->capturedFields)
        record.capturedFields.push_back({field.name, referenceOf(field.type)});
    for (const auto& variant : type->variants) {
        TypeVariantRecord frozen;
        frozen.name = variant.name;
        for (const auto& field : variant.fields)
            frozen.fields.push_back(referenceOf(field));
        record.variants.push_back(std::move(frozen));
    }
    addReference(type->inner);
    addReference(type->returnType);
    for (const auto& argument : type->typeArgs) addReference(argument);
    for (const auto& parameter : type->paramTypes) addReference(parameter);
    for (const auto& field : type->fields) addReference(field.type);
    for (const auto& field : type->capturedFields) addReference(field.type);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields) addReference(field);

    const auto payloadRank = [](const TypeRecord& candidate) {
        size_t rank = candidate.typeArgumentIds.size() +
            candidate.parameterTypeIds.size() + candidate.fields.size() +
            candidate.capturedFields.size();
        if (!candidate.innerTypeId.empty()) ++rank;
        if (!candidate.returnTypeId.empty()) ++rank;
        for (const auto& variant : candidate.variants)
            rank += 1 + variant.fields.size();
        return rank;
    };
    for (auto& existing : typeTable) {
        if (existing.id != id) continue;
        if (existing.canonicalType != canonicalTypeValue)
            throw std::logic_error(
                "colliding canonical payloads for MoonIR TypeId '" +
                id.value + "'");
        if (existing.canonicalShape == record.canonicalShape) return id;
        const size_t existingRank = payloadRank(existing);
        const size_t candidateRank = payloadRank(record);
        if (candidateRank < existingRank) return id;
        if (candidateRank == existingRank)
            throw std::logic_error(
                "conflicting frozen payloads for MoonIR TypeId '" +
                id.value + "'");

        // A nominal forward placeholder and its completed declaration share
        // one TypeId. Prefer the strictly richer payload regardless of
        // registration order, then close any newly exposed graph edges.
        existing = std::move(record);
        registerType(type->inner);
        registerType(type->returnType);
        for (const auto& argument : type->typeArgs) registerType(argument);
        for (const auto& parameter : type->paramTypes) registerType(parameter);
        for (const auto& field : type->fields) registerType(field.type);
        for (const auto& field : type->capturedFields)
            registerType(field.type);
        for (const auto& variant : type->variants)
            for (const auto& field : variant.fields) registerType(field);
        return id;
    }
    typeTable.push_back(std::move(record));

    registerType(type->inner);
    registerType(type->returnType);
    for (const auto& argument : type->typeArgs) registerType(argument);
    for (const auto& parameter : type->paramTypes) registerType(parameter);
    for (const auto& field : type->fields) registerType(field.type);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields) registerType(field);
    return id;
}

void Module::sealTypeTable() {
    if (typeTableSealed) return;
    std::sort(typeTable.begin(), typeTable.end(),
              [](const TypeRecord& left, const TypeRecord& right) {
                  return left.id.value < right.id.value;
              });
    typeTableSealed = true;
    rebuildIndexes();

    // Forward nominal placeholders may be distinct frontend objects from
    // their completed declarations. Normalize every derived identity/layout
    // from the closed frozen graph, never from that construction topology.
    TypeMaterializer materializer(*this);
    for (auto& record : typeTable) {
        const TypePtr restored = materializer.materialize(record.id);
        if (!restored || luna::types::canonicalType(restored) !=
                record.canonicalType ||
            luna::types::typeId(restored) != record.id)
            throw std::logic_error(
                "frozen MoonIR payload changes TypeId '" +
                record.id.value + "' during sealing");

        record.canonicalShape = luna::types::canonicalShape(restored);
        record.shapeId = luna::types::shapeId(restored);
        record.valueSize = luna::layout::valueSize(restored);
        record.valueAlignment = luna::layout::valueAlignment(restored);
        record.layoutAbiVersion = 0;
        record.abiLayout.clear();
        if (record.kind == TypeKind::Enum ||
            record.kind == TypeKind::Result) {
            record.layoutAbiVersion = luna::layout::InlineAdtAbiVersion;
            record.abiLayout =
                luna::layout::inlineAdtLayoutSignature(restored);
        }
        record.canonicalAbiLayout = canonicalAbiLayout(record);
        record.abiLayoutId = luna::identity::abiLayoutIdFromCanonical(
            record.canonicalAbiLayout);
        const auto resource = resourceContractForType(restored);
        record.sysmeta.resource.usage = resource.usage;
        record.sysmeta.resource.cleanup = resource.cleanup;
        record.sysmeta.resource.cleanupRequired = resource.cleanupRequired;
        record.sysmeta.resource.recursiveCleanup = resource.recursiveCleanup;
        record.sysmeta.resource.lifetime = resource.lifetime;
        record.sysmeta.resource.relation = resource.relation;
        record.sysmeta.identity.type = record.id;
        record.sysmeta.identity.shape = record.shapeId;
        record.sysmeta.identity.abiLayout = record.abiLayoutId;
    }

    // Declaration contracts are part of the same sealed snapshot. A
    // declaration may have been lowered while one of its nominal children
    // was still a forward placeholder, so refresh the type-derived portions
    // after the type graph has reached its canonical closed form.
    for (auto& declaration : declarationTable) {
        if (const auto* type = findType(declaration.type)) {
            declaration.sysmeta.resource = type->sysmeta.resource;
            if (type->kind == TypeKind::Slot ||
                type->kind == TypeKind::Fragment)
                declaration.sysmeta.control = type->sysmeta.control;
        }
        declaration.canonicalContract = canonicalContract(declaration);
        declaration.contractId = luna::identity::contractIdFromCanonical(
            declaration.canonicalContract);
        declaration.sysmeta.identity.symbol = declaration.symbolId;
        declaration.sysmeta.identity.contract = declaration.contractId;
    }
}

const TypeRecord* Module::findType(const TypeRef& id) const {
    if (!typeTableSealed) {
        for (const auto& type : typeTable)
            if (type.id == id) return &type;
        return nullptr;
    }
    auto found = typesById.find(id.value);
    if (found == typesById.end() || found->second >= typeTable.size()) return nullptr;
    return &typeTable[found->second];
}

const BasicBlock* ControlFlowGraph::findBlock(BlockId id) const {
    return !id.empty() && id.value < blocks.size() &&
                   blocks[id.value].id == id
        ? &blocks[id.value] : nullptr;
}

const RegionRecord* ControlFlowGraph::findRegion(RegionId id) const {
    return !id.empty() && id.value < regions.size() &&
                   regions[id.value].id == id
        ? &regions[id.value] : nullptr;
}

const ScopeRecord* ControlFlowGraph::findScope(ScopeId id) const {
    return !id.empty() && id.value < scopes.size() &&
                   scopes[id.value].id == id
        ? &scopes[id.value] : nullptr;
}

const LocalRecord* ControlFlowGraph::findLocal(LocalId id) const {
    return !id.empty() && id.value < locals.size() &&
                   locals[id.value].id == id
        ? &locals[id.value] : nullptr;
}

const CleanupRecord* ControlFlowGraph::findCleanup(CleanupId id) const {
    return !id.empty() && id.value < cleanups.size() &&
                   cleanups[id.value].id == id
        ? &cleanups[id.value] : nullptr;
}

const DeclarationRecord* Module::findDeclaration(
    const SymbolRef& symbol) const {
    auto found = declarationRecordsBySymbol.find(symbol.value);
    if (found == declarationRecordsBySymbol.end() ||
        found->second >= declarationTable.size())
        return nullptr;
    return &declarationTable[found->second];
}

const DeclarationRecord* Module::findDeclaration(
    const DeclarationRef& reference) const {
    const auto* declaration = findDeclaration(reference.symbol);
    return declaration && declaration->contractId == reference.contract
        ? declaration : nullptr;
}

const DeclarationRecord* Module::findDeclarationById(
    const std::string& id) const {
    auto found = declarationRecordsById.find(id);
    if (found == declarationRecordsById.end() ||
        found->second >= declarationTable.size())
        return nullptr;
    return &declarationTable[found->second];
}

const DeclarationRecord* Module::findDeclarationByLinkage(
    const std::string& linkage) const {
    auto found = declarationRecordsByLinkage.find(linkage);
    if (found == declarationRecordsByLinkage.end() ||
        found->second >= declarationTable.size())
        return nullptr;
    return &declarationTable[found->second];
}

TypePtr TypeMaterializer::materialize(const TypeRef& reference) {
    if (reference.empty()) return nullptr;
    if (auto found = mCache.find(reference.value); found != mCache.end())
        return found->second;
    const auto* record = mModule.findType(reference);
    if (!record) return nullptr;

    // Publish the skeleton before following graph edges so recursive nominal
    // types resolve to the same backend object without frontend pointer
    // identity participating in the process.
    auto result = std::make_shared<Type>();
    mCache.emplace(reference.value, result);
    result->kind = record->kind;
    result->domain = record->domain;
    result->identityMode = record->identityMode;
    result->name = record->sourceName;
    result->declarationLinkageName = record->declarationLinkageName;
    result->nominalId = record->nominalDeclarationId;
    result->typeParams = record->typeParameterNames;
    result->arrayLength = record->arrayLength;
    result->isMutable = record->isMutable;
    result->paramContracts = record->parameterContracts;
    result->returnContract = record->returnContract;
    result->sysmeta = record->sysmeta;
    result->isMultiShot = record->isMultiShot;
    result->continuationKind = record->continuationKind;
    result->iteratorMode = record->iteratorMode;
    result->inferenceId = record->inferenceId;

    result->inner = materialize(record->innerTypeId);
    result->returnType = materialize(record->returnTypeId);
    for (const auto& argument : record->typeArgumentIds)
        result->typeArgs.push_back(materialize(argument));
    for (const auto& parameter : record->parameterTypeIds)
        result->paramTypes.push_back(materialize(parameter));
    for (const auto& field : record->fields)
        result->fields.push_back({field.name, materialize(field.type)});
    for (const auto& field : record->capturedFields)
        result->capturedFields.push_back(
            {field.name, materialize(field.type)});
    for (const auto& variant : record->variants) {
        TypeVariant restored;
        restored.name = variant.name;
        for (const auto& field : variant.fields)
            restored.fields.push_back(materialize(field));
        result->variants.push_back(std::move(restored));
    }
    return result;
}

const char* retentionName(Retention retention) {
    switch (retention) {
        case Retention::CompileTime: return "compile_time";
        case Retention::Runtime: return "runtime";
        case Retention::Dynamic: return "dynamic";
    }
    return "unknown";
}

const char* declarationKindName(DeclarationKind kind) {
    switch (kind) {
        case DeclarationKind::Function: return "function";
        case DeclarationKind::Fragment: return "fragment";
        case DeclarationKind::Struct: return "struct";
        case DeclarationKind::Enum: return "enum";
        case DeclarationKind::Trait: return "trait";
        case DeclarationKind::Implementation: return "implementation";
        case DeclarationKind::MetadataSchema: return "metadata_schema";
    }
    return "unknown";
}

const char* costKindName(CostKind kind) {
    switch (kind) {
        case CostKind::MachineCode: return "machine_code";
        case CostKind::GenericInstantiation: return "generic_instantiation";
        case CostKind::RuntimeDescriptor: return "runtime_descriptor";
        case CostKind::RuntimeMetadata: return "runtime_metadata";
        case CostKind::DynamicBinding: return "dynamic_binding";
        case CostKind::KernelCode: return "kernel_code";
        case CostKind::ReservedCapability: return "reserved_capability";
    }
    return "unknown";
}

const char* operatorName(Operator op) {
    switch (op) {
        case Operator::Add: return "add";
        case Operator::Subtract: return "sub";
        case Operator::Multiply: return "mul";
        case Operator::Divide: return "div";
        case Operator::Remainder: return "rem";
        case Operator::Assign: return "assign";
        case Operator::AddAssign: return "add_assign";
        case Operator::SubtractAssign: return "sub_assign";
        case Operator::MultiplyAssign: return "mul_assign";
        case Operator::DivideAssign: return "div_assign";
        case Operator::RemainderAssign: return "rem_assign";
        case Operator::BitAndAssign: return "and_assign";
        case Operator::BitOrAssign: return "or_assign";
        case Operator::BitXorAssign: return "xor_assign";
        case Operator::ShiftLeftAssign: return "shl_assign";
        case Operator::ShiftRightAssign: return "shr_assign";
        case Operator::Equal: return "eq";
        case Operator::NotEqual: return "ne";
        case Operator::Less: return "lt";
        case Operator::LessEqual: return "le";
        case Operator::Greater: return "gt";
        case Operator::GreaterEqual: return "ge";
        case Operator::ShiftLeft: return "shl";
        case Operator::ShiftRight: return "shr";
        case Operator::LogicalAnd: return "logical_and";
        case Operator::LogicalOr: return "logical_or";
        case Operator::BitAnd: return "bit_and";
        case Operator::BitOr: return "bit_or";
        case Operator::BitXor: return "bit_xor";
        case Operator::LogicalNot: return "logical_not";
        case Operator::BitNot: return "bit_not";
        case Operator::Dereference: return "deref";
        case Operator::Negate: return "neg";
    }
    return "unknown";
}

const char* regionKindName(RegionKind kind) {
    switch (kind) {
        case RegionKind::Function: return "function";
        case RegionKind::Lambda: return "lambda";
        case RegionKind::Fragment: return "fragment";
        case RegionKind::Continuation: return "continuation";
        case RegionKind::Lexical: return "lexical";
        case RegionKind::Loop: return "loop";
        case RegionKind::MatchArm: return "match_arm";
        case RegionKind::Apply: return "apply";
    }
    return "unknown";
}

const char* terminatorKindName(TerminatorKind kind) {
    switch (kind) {
        case TerminatorKind::Invalid: return "invalid";
        case TerminatorKind::Jump: return "jump";
        case TerminatorKind::Branch: return "branch";
        case TerminatorKind::Switch: return "switch";
        case TerminatorKind::Return: return "return";
        case TerminatorKind::Resume: return "resume";
        case TerminatorKind::Abort: return "abort";
        case TerminatorKind::Unreachable: return "unreachable";
    }
    return "unknown";
}

const char* typeDomainName(luna::types::TypeDomain domain) {
    switch (domain) {
        case luna::types::TypeDomain::Value: return "value";
        case luna::types::TypeDomain::Meta: return "meta";
        case luna::types::TypeDomain::Compiler: return "compiler";
        case luna::types::TypeDomain::Inference: return "inference";
        case luna::types::TypeDomain::Error: return "error";
    }
    return "invalid";
}

const char* identityModeName(luna::types::IdentityMode mode) {
    switch (mode) {
        case luna::types::IdentityMode::Structural: return "structural";
        case luna::types::IdentityMode::Nominal: return "nominal";
        case luna::types::IdentityMode::Builtin: return "builtin";
        case luna::types::IdentityMode::MetaSchema: return "meta_schema";
        case luna::types::IdentityMode::CompilerIntrinsic: return "compiler_intrinsic";
        case luna::types::IdentityMode::Inference: return "inference";
        case luna::types::IdentityMode::Error: return "error";
    }
    return "invalid";
}

} // namespace moon
