#include "MoonIR.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <stdexcept>

namespace moon {

void Module::rebuildIndexes() {
    typesById.clear();
    for (size_t index = 0; index < typeTable.size(); ++index)
        typesById[typeTable[index].id.value] = index;
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

void Module::registerType(const TypePtr& type) {
    if (!type) return;
    if (typeTableSealed)
        throw std::logic_error("cannot register a type after the MoonIR type table is sealed");
    const auto canonicalTypeValue = luna::types::canonicalType(type);
    const auto id = luna::types::typeId(type);
    for (const auto& existing : typeTable) {
        if (existing.id == id && existing.canonicalType == canonicalTypeValue)
            return;
    }

    TypeRecord record;
    record.id = id;
    record.shapeId = luna::types::shapeId(type);
    record.domain = type->domain;
    record.identityMode = type->identityMode;
    record.kind = type->kind;
    record.sysmeta = type->sysmeta;
    record.displayName = type->toString();
    record.nominalDeclarationId = type->nominalId;
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
    const auto addReference = [&](const TypePtr& referenced) {
        if (referenced) record.referencedTypeIds.push_back(luna::types::typeId(referenced));
    };
    addReference(type->inner);
    addReference(type->returnType);
    for (const auto& argument : type->typeArgs) addReference(argument);
    for (const auto& parameter : type->paramTypes) addReference(parameter);
    for (const auto& field : type->fields) addReference(field.type);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields) addReference(field);
    typeTable.push_back(std::move(record));

    registerType(type->inner);
    registerType(type->returnType);
    for (const auto& argument : type->typeArgs) registerType(argument);
    for (const auto& parameter : type->paramTypes) registerType(parameter);
    for (const auto& field : type->fields) registerType(field.type);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields) registerType(field);
}

void Module::sealTypeTable() {
    if (typeTableSealed) return;
    std::sort(typeTable.begin(), typeTable.end(),
              [](const TypeRecord& left, const TypeRecord& right) {
                  return left.id.value < right.id.value;
              });
    typeTableSealed = true;
    rebuildIndexes();
}

const TypeRecord* Module::findType(const luna::types::TypeId& id) const {
    auto found = typesById.find(id.value);
    if (found == typesById.end() || found->second >= typeTable.size()) return nullptr;
    return &typeTable[found->second];
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
