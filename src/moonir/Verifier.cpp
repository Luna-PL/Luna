#include "Verifier.h"
#include "../core/TypeLayout.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace moon {

void Verifier::verifyCleanupAction(
    luna::ownership::CleanupAction action,
    const luna::types::TypeId& typeId,
    const SourceLocation& location,
    const std::string& context,
    const Module& module) {
    const auto* type = module.findType(typeId);
    if (!type) return;
    if (!type->sysmeta.resource.cleanupRequired &&
        action != luna::ownership::CleanupAction::Deallocate)
        error(location, context + " targets a type with no cleanup obligation");
    if (type->sysmeta.resource.cleanupRequired &&
        action != type->sysmeta.resource.cleanup)
        error(location, context + " uses cleanup action '" +
              std::string(luna::ownership::cleanupActionName(action)) +
              "' but its frozen ResourceContract requires '" +
              std::string(luna::ownership::cleanupActionName(
                  type->sysmeta.resource.cleanup)) + "'");
}

namespace {

bool isGeneric(const FunctionDecl& function) {
    return !function.typeParams.empty() && !function.isTemplateInstance;
}

bool isIntegerMetadataType(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize:
            return true;
        default:
            return false;
    }
}

bool metadataConstantMatches(const ConstantValue& value, const TypeRecord* type) {
    if (!type) return false;
    if (std::holds_alternative<int64_t>(value))
        return isIntegerMetadataType(type->kind);
    if (std::holds_alternative<double>(value))
        return type->kind == TypeKind::F32 || type->kind == TypeKind::F64;
    if (std::holds_alternative<bool>(value))
        return type->kind == TypeKind::Bool;
    if (std::holds_alternative<std::string>(value))
        return type->kind == TypeKind::String || type->kind == TypeKind::CStr;
    return false;
}

luna::ownership::Usage frozenUsage(
    const Module& module, const TypeRef& reference) {
    const auto* type = module.findType(reference);
    return type ? type->sysmeta.resource.usage
                : luna::ownership::Usage::Copy;
}

bool validIdentifier(const std::string& value) {
    if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value[0])) &&
                          value[0] != '_'))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

bool validSeparatedName(const std::string& value, const std::string& separator,
                        bool emptyAllowed = false) {
    if (value.empty()) return emptyAllowed;
    size_t begin = 0;
    for (;;) {
        const size_t end = value.find(separator, begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!validIdentifier(component)) return false;
        if (end == std::string::npos) return true;
        begin = end + separator.size();
    }
}

} // namespace

bool Verifier::verify(const Module& module) {
    mErrors.clear();
    mVerifiedTypeIds.clear();
    mActiveTypeIds.clear();
    if (module.formatMajor != FormatMajor) {
        error({}, "unsupported MoonIR major version " +
                  std::to_string(module.formatMajor));
    }
    if (!module.typeTableSealed)
        error({}, "MoonIR type table must be sealed before verification");
    if (!validSeparatedName(module.name, "."))
        error({}, "module has an invalid canonical Package ID '" + module.name + "'");
    std::unordered_set<std::string> sourceModules;
    for (const auto& sourceModule : module.sourceModules) {
        if (!validSeparatedName(sourceModule, "::", true))
            error({}, "invalid source module path '" + sourceModule + "'");
        if (!sourceModules.insert(sourceModule).second)
            error({}, "duplicate source module path '" + sourceModule + "'");
    }
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        packageAliases;
    for (const auto& use : module.packageUses) {
        if (!validSeparatedName(use.ownerPackageId, ".") ||
            !validSeparatedName(use.packageId, ".") || !validIdentifier(use.alias)) {
            error({}, "invalid package using entry '" + use.packageId + "' as '" +
                      use.alias + "'");
            continue;
        }
        if (use.packageId == use.ownerPackageId)
            error({}, "package cannot use itself as '" + use.alias + "'");
        auto [found, inserted] = packageAliases[use.ownerPackageId].emplace(
            use.alias, use.packageId);
        if (!inserted && found->second != use.packageId)
            error({}, "package alias '" + use.alias + "' identifies multiple packages");
    }

    std::unordered_map<std::string, std::string> shapePayloads;
    std::unordered_map<std::string, std::string> abiLayoutPayloads;
    for (const auto& type : module.typeTable) {
        if (type.id.empty()) {
            error({}, "type table entry has no stable TypeId");
            continue;
        }
        if (!mVerifiedTypeIds.insert(type.id.value).second)
            error({}, "duplicate or colliding TypeId '" + type.id.value + "'");
        if (luna::types::typeIdFromCanonical(type.canonicalType) != type.id)
            error({}, "TypeId payload mismatch for '" + type.id.value + "'");
        if (luna::types::shapeIdFromCanonical(type.canonicalShape) != type.shapeId)
            error({}, "ShapeId payload mismatch for type '" + type.id.value + "'");
        auto [shape, insertedShape] = shapePayloads.emplace(
            type.shapeId.value, type.canonicalShape);
        if (!insertedShape && shape->second != type.canonicalShape)
            error({}, "colliding ShapeId '" + type.shapeId.value + "'");
        if (canonicalAbiLayout(type) != type.canonicalAbiLayout)
            error({}, "ABI layout payload mismatch for type '" + type.id.value + "'");
        if (luna::identity::abiLayoutIdFromCanonical(
                type.canonicalAbiLayout) != type.abiLayoutId)
            error({}, "AbiLayoutId payload mismatch for type '" +
                      type.id.value + "'");
        auto [layout, insertedLayout] = abiLayoutPayloads.emplace(
            type.abiLayoutId.value, type.canonicalAbiLayout);
        if (!insertedLayout && layout->second != type.canonicalAbiLayout)
            error({}, "colliding AbiLayoutId '" +
                      type.abiLayoutId.value + "'");
        if (type.sysmeta.identity.type != type.id ||
            type.sysmeta.identity.shape != type.shapeId ||
            type.sysmeta.identity.abiLayout != type.abiLayoutId)
            error({}, "type '" + type.id.value +
                      "' has inconsistent identity sysmeta");
        if (type.domain == luna::types::TypeDomain::Inference ||
            type.domain == luna::types::TypeDomain::Error ||
            type.identityMode == luna::types::IdentityMode::Inference ||
            type.identityMode == luna::types::IdentityMode::Error)
            error({}, "unresolved Sema type entered MoonIR type table as '" +
                      type.id.value + "' (" + type.displayName + ")");
        if ((type.identityMode == luna::types::IdentityMode::Nominal ||
             type.identityMode == luna::types::IdentityMode::MetaSchema) &&
            type.nominalDeclarationId.empty())
            error({}, "identity-bearing type '" + type.id.value +
                      "' has no declaration identity");
        if (type.identityMode == luna::types::IdentityMode::Structural &&
            !type.nominalDeclarationId.empty())
            error({}, "structural type '" + type.id.value +
                      "' unexpectedly carries a nominal identity");
        if (type.valueAlignment == 0 ||
            (type.valueAlignment & (type.valueAlignment - 1)) != 0)
            error({}, "type '" + type.id.value +
                      "' has an invalid ABI alignment");
        if (type.kind == TypeKind::Enum ||
            type.kind == TypeKind::Result) {
            if (type.layoutAbiVersion !=
                luna::layout::InlineAdtAbiVersion)
                error({}, "inline ADT '" + type.id.value +
                          "' uses an unsupported layout ABI");
            const std::string prefix =
                "luna.inline-adt.v" +
                std::to_string(luna::layout::InlineAdtAbiVersion) +
                ";tag_storage=" +
                std::to_string(luna::layout::InlineTagStorageSize) +
                ";payload_align=" +
                std::to_string(luna::layout::InlinePayloadAlignment) +
                ";size=" + std::to_string(type.valueSize);
            if (type.abiLayout.rfind(prefix, 0) != 0)
                error({}, "inline ADT '" + type.id.value +
                          "' has an inconsistent frozen ABI signature");
        } else if (type.layoutAbiVersion != 0 ||
                   !type.abiLayout.empty()) {
            error({}, "non-ADT type '" + type.id.value +
                      "' unexpectedly carries inline ADT ABI metadata");
        }
        if (type.kind == TypeKind::Metadata &&
            (type.domain != luna::types::TypeDomain::Meta ||
             type.identityMode != luna::types::IdentityMode::MetaSchema))
            error({}, "metadata type '" + type.id.value +
                      "' is outside the Meta type domain");
        if ((type.kind == TypeKind::MetadataView ||
             type.kind == TypeKind::DeclarationView ||
             type.kind == TypeKind::DeclarationRef) &&
            type.domain != luna::types::TypeDomain::Compiler)
            error({}, "compile-time view/reference type '" + type.id.value +
                      "' is outside the Compiler type domain");
        if (type.kind == TypeKind::Trait &&
            type.domain != luna::types::TypeDomain::Compiler)
            error({}, "trait type '" + type.id.value +
                      "' is outside the Compiler type domain");
        if (type.sysmeta.schemaMajor != luna::sysmeta::SchemaMajor)
            error({}, "type '" + type.id.value +
                      "' uses an unsupported sysmeta schema");
        if (type.kind == TypeKind::DeviceBuffer &&
            type.sysmeta.resource.releaseDomain !=
                luna::sysmeta::ReleaseDomain::Device)
            error({}, "device_buffer type '" + type.id.value +
                      "' has inconsistent release-domain sysmeta");
        if (type.kind == TypeKind::Result) {
            if (type.referencedTypeIds.size() != 2) {
                error({}, "Result type '" + type.id.value +
                          "' must reference exactly value and error payload types");
            }
        }
        if (type.sysmeta.resource.needsDrop !=
            !type.sysmeta.abi.dropGlueSymbol.empty())
            error({}, "type '" + type.id.value +
                      "' has inconsistent Drop sysmeta");
        if (type.sysmeta.resource.cleanupRequired !=
            (type.sysmeta.resource.cleanup !=
             luna::ownership::CleanupAction::None))
            error({}, "type '" + type.id.value +
                      "' has inconsistent Resource cleanup sysmeta");
        if (type.sysmeta.resource.cleanupRequired &&
            type.sysmeta.resource.usage ==
                luna::ownership::Usage::Copy)
            error({}, "type '" + type.id.value +
                      "' has cleanup obligations but Copy usage");
        if (type.sysmeta.resource.recursiveCleanup &&
            !type.sysmeta.resource.cleanupRequired)
            error({}, "type '" + type.id.value +
                      "' has recursive cleanup without a cleanup obligation");
        if (type.kind == TypeKind::Reference &&
            (type.sysmeta.resource.lifetime !=
                 luna::sysmeta::ResourceLifetime::Borrowed ||
             type.sysmeta.resource.relation ==
                 luna::ownership::Relation::Owned))
            error({}, "reference type '" + type.id.value +
                      "' has inconsistent borrowed Resource lifetime");
        if (type.sysmeta.resource.lifetime ==
                luna::sysmeta::ResourceLifetime::Borrowed &&
            type.kind != TypeKind::Reference)
            error({}, "non-reference type '" + type.id.value +
                      "' claims a borrowed Resource lifetime");
        if (type.sysmeta.resource.cleanupRequired &&
            type.sysmeta.resource.lifetime ==
                luna::sysmeta::ResourceLifetime::Value)
            error({}, "type '" + type.id.value +
                      "' has cleanup obligations but value lifetime");
    }
    for (const auto& type : module.typeTable) {
        std::vector<TypeRef> structuralReferences;
        const auto appendReference = [&](const TypeRef& reference) {
            if (!reference.empty()) structuralReferences.push_back(reference);
        };
        appendReference(type.innerTypeId);
        appendReference(type.returnTypeId);
        for (const auto& reference : type.typeArgumentIds)
            appendReference(reference);
        for (const auto& reference : type.parameterTypeIds)
            appendReference(reference);
        for (const auto& field : type.fields)
            appendReference(field.type);
        for (const auto& variant : type.variants)
            for (const auto& field : variant.fields)
                appendReference(field);
        if (structuralReferences != type.referencedTypeIds)
            error({}, "frozen type '" + type.id.value +
                      "' has a reference index inconsistent with its payload");
        for (const auto& referenced : type.referencedTypeIds) {
            if (!module.findType(referenced))
                error({}, "frozen type '" + type.id.value +
                      "' references missing type '" + referenced.value + "'");
        }
    }

    // Reconstruct from frozen records only, then recompute every identity.
    // This proves the verifier does not rely on the frontend Type objects that
    // happened to exist while the module was built.
    TypeMaterializer materializer(module);
    for (const auto& type : module.typeTable) {
        const TypePtr restored = materializer.materialize(type.id);
        if (!restored) {
            error({}, "cannot materialize frozen type '" + type.id.value + "'");
            continue;
        }
        if (luna::types::canonicalType(restored) != type.canonicalType ||
            luna::types::typeId(restored) != type.id)
            error({}, "frozen payload does not reproduce TypeId '" +
                      type.id.value + "'");
        const auto restoredShape = luna::types::canonicalShape(restored);
        if (restoredShape != type.canonicalShape ||
            luna::types::shapeId(restored) != type.shapeId)
            error({}, "frozen payload does not reproduce ShapeId for type '" +
                      type.id.value + "' (" + type.displayName +
                      ")");
        if (luna::layout::valueSize(restored) != type.valueSize ||
            luna::layout::valueAlignment(restored) != type.valueAlignment)
            error({}, "frozen payload does not reproduce layout for type '" +
                      type.id.value + "'");
        if ((type.kind == TypeKind::Enum || type.kind == TypeKind::Result) &&
            luna::layout::inlineAdtLayoutSignature(restored) != type.abiLayout)
            error({}, "frozen payload does not reproduce inline ADT layout for type '" +
                      type.id.value + "'");
        const auto resource = resourceContractForType(restored);
        if (resource.usage != type.sysmeta.resource.usage ||
            resource.cleanup != type.sysmeta.resource.cleanup ||
            resource.cleanupRequired !=
                type.sysmeta.resource.cleanupRequired ||
            resource.recursiveCleanup !=
                type.sysmeta.resource.recursiveCleanup ||
            resource.lifetime != type.sysmeta.resource.lifetime ||
            resource.relation != type.sysmeta.resource.relation)
            error({}, "frozen payload does not reproduce Resource contract for type '" +
                      type.id.value + "'");
    }

    std::unordered_set<std::string> schemaIds;
    std::unordered_map<std::string, const MetadataSchema*> schemasById;
    for (const auto& schema : module.metadataSchemas) {
        if (schema.id.empty()) error(schema.location, "metadata schema has no stable id");
        else if (!schemaIds.insert(schema.id).second) {
            error(schema.location, "duplicate metadata schema id '" + schema.id + "'");
        } else {
            schemasById.emplace(schema.id, &schema);
        }
        if (schema.fields.empty())
            error(schema.location, "metadata schema '" + schema.name + "' has no fields");
        std::unordered_set<std::string> fieldNames;
        for (const auto& field : schema.fields) {
            if (field.name.empty())
                error(schema.location, "metadata schema '" + schema.name +
                                       "' contains an unnamed field");
            else if (!fieldNames.insert(field.name).second)
                error(schema.location, "metadata schema '" + schema.name +
                                       "' contains duplicate field '" + field.name + "'");
            verifyType(field.type, schema.location,
                       "metadata field '" + schema.name + "." + field.name + "'",
                       module);
        }
    }

    std::unordered_set<std::string> declarationIds;
    std::unordered_set<std::string> linkageNames;
    std::unordered_set<std::string> symbolIds;
    std::unordered_map<std::string, std::string> contractPayloads;
    std::unordered_map<std::string, const DeclarationRecord*> recordsById;
    for (const auto& record : module.declarationTable) {
        if (record.id.empty()) error(record.location, "declaration table entry has no id");
        else if (!declarationIds.insert(record.id).second) {
            error(record.location, "duplicate declaration id '" + record.id + "'");
        } else {
            recordsById.emplace(record.id, &record);
        }
        if (luna::identity::symbolIdFromCanonical(record.id) !=
            record.symbolId)
            error(record.location, "SymbolId payload mismatch for declaration '" +
                                   record.id + "'");
        else if (!symbolIds.insert(record.symbolId.value).second)
            error(record.location, "duplicate or colliding SymbolId '" +
                                   record.symbolId.value + "'");
        if (canonicalContract(record) != record.canonicalContract)
            error(record.location, "contract payload mismatch for declaration '" +
                                   record.id + "'");
        if (luna::identity::contractIdFromCanonical(
                record.canonicalContract) != record.contractId)
            error(record.location, "ContractId payload mismatch for declaration '" +
                                   record.id + "'");
        auto [contract, insertedContract] = contractPayloads.emplace(
            record.contractId.value, record.canonicalContract);
        if (!insertedContract &&
            contract->second != record.canonicalContract)
            error(record.location, "colliding ContractId '" +
                                   record.contractId.value + "'");
        if (record.sysmeta.identity.symbol != record.symbolId ||
            record.sysmeta.identity.contract != record.contractId)
            error(record.location, "declaration '" + record.id +
                                   "' has inconsistent identity sysmeta");
        if (!record.linkageName.empty() &&
            !linkageNames.insert(record.linkageName).second)
            error(record.location, "duplicate linkage identity '" +
                                   record.linkageName + "'");
        if (record.retention != Retention::CompileTime && !module.features.runtime)
            error(record.location, "runtime-retained declaration '" + record.id +
                                   "' is present without the runtime feature");
        if (record.sysmeta.schemaMajor != luna::sysmeta::SchemaMajor)
            error(record.location, "declaration '" + record.id +
                                   "' uses an unsupported sysmeta schema");
        if (record.sysmeta.capability.dynamicDispatch &&
            !module.features.dynamicApply)
            error(record.location, "declaration '" + record.id +
                                   "' requires dynamic dispatch without the module capability");
        if (record.sysmeta.capability.runtimeRetained !=
            (record.retention != Retention::CompileTime))
            error(record.location, "declaration '" + record.id +
                                   "' has inconsistent runtime-retention sysmeta");
        if (!record.type.empty()) {
            const auto* recordType = module.findType(record.type);
            if (!recordType)
                error(record.location, "declaration '" + record.id +
                                       "' references a type absent from the type table");
            else if ((recordType->kind == TypeKind::Function ||
                      recordType->kind == TypeKind::Slot ||
                      recordType->kind == TypeKind::Fragment) &&
                record.sysmeta.resource.parameters.size() !=
                    recordType->parameterTypeIds.size())
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta parameter contract count does not match its type");
            const size_t contractCount = std::min(
                record.sysmeta.resource.parameters.size(),
                recordType ? recordType->parameterContracts.size() : size_t{0});
            for (size_t index = 0; index < contractCount; ++index) {
                if (record.sysmeta.resource.parameters[index] !=
                    recordType->parameterContracts[index])
                    error(record.location, "declaration '" + record.id +
                                           "' sysmeta ownership contract differs from its type");
            }
            if (recordType &&
                record.sysmeta.resource.result != recordType->returnContract)
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta result contract differs from its type");
        }
        if (record.kind == DeclarationKind::Fragment) {
            const auto form = record.sysmeta.control.form;
            if (form != luna::sysmeta::ControlForm::Interceptor &&
                form != luna::sysmeta::ControlForm::Context)
                error(record.location, "fragment '" + record.id +
                                       "' has no fragment control sysmeta");
            if (record.sysmeta.control.storage !=
                luna::sysmeta::ContinuationStorage::ScopedStack)
                error(record.location, "fragment '" + record.id +
                                       "' must use a scoped continuation in the current ABI");
            if (!record.sysmeta.capability.hostOnly)
                error(record.location, "fragment '" + record.id +
                                       "' must be host-only in the current ABI");
        }
        for (const auto& metadata : record.metadata) {
            auto schema = schemasById.find(metadata.schemaId);
            if (schema == schemasById.end()) {
                error(metadata.location, "metadata instance references unknown schema '" +
                                         metadata.schemaId + "'");
            } else {
                const auto& fields = schema->second->fields;
                if (metadata.values.size() != fields.size()) {
                    error(metadata.location, "metadata instance for schema '" +
                                             metadata.schemaId + "' has " +
                                             std::to_string(metadata.values.size()) +
                                             " values; expected " +
                                             std::to_string(fields.size()));
                }
                const size_t comparable = std::min(metadata.values.size(), fields.size());
                for (size_t index = 0; index < comparable; ++index) {
                    if (!metadataConstantMatches(
                            metadata.values[index],
                            module.findType(fields[index].type)))
                        error(metadata.location, "metadata value for field '" +
                                                 fields[index].name +
                                                 "' does not match its schema type");
                }
            }
            if (metadata.retention != Retention::CompileTime && !module.features.runtime)
                error(metadata.location, "runtime metadata is present without the runtime feature");
            if (static_cast<uint8_t>(metadata.retention) >
                static_cast<uint8_t>(record.retention))
                error(metadata.location, "metadata retention exceeds declaration retention for '" +
                                         record.id + "'");
        }
    }

    std::unordered_set<std::string> executableIds;
    for (const auto& declaration : module.declarations) {
        if (!declaration) {
            error({}, "module contains a null declaration");
            continue;
        }
        if (declaration->declarationId.empty())
            error(declaration->location, "executable declaration has no stable id");
        else if (!executableIds.insert(declaration->declarationId).second)
            error(declaration->location, "duplicate executable declaration id '" +
                                         declaration->declarationId + "'");
        auto record = recordsById.find(declaration->declarationId);
        if (record == recordsById.end()) {
            error(declaration->location, "executable declaration '" +
                                         declaration->declarationId +
                                         "' is absent from the declaration table");
        } else {
            const auto& linkage = declaration->generatedSymbolName;
            if (declaration->symbolId != record->second->symbolId)
                error(declaration->location, "declaration table SymbolId mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->linkageName != linkage)
                error(declaration->location, "declaration table linkage mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->familyId != declaration->familyId)
                error(declaration->location, "declaration table family mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->sysmeta.schemaMajor !=
                declaration->sysmeta.schemaMajor)
                error(declaration->location, "declaration sysmeta schema mismatch for '" +
                                             declaration->declarationId + "'");
        }
        verifyDeclaration(*declaration, module);
    }

    if (module.features.dynamicApply && !module.features.runtime)
        error({}, "dynamic apply requires the runtime feature");
    if (module.features.dynamicSelect && !module.features.runtime)
        error({}, "dynamic select requires the runtime feature");
    if (module.features.dynamicReflection && !module.features.runtime)
        error({}, "dynamic reflection requires the runtime feature");
    if (module.features.kernelRuntimeReserved && !module.features.kernel)
        error({}, "reserved kernel runtime requires the kernel feature");

    return mErrors.empty();
}

void Verifier::verifyDeclaration(const Decl& declaration, const Module& module) {
    if (!validSeparatedName(declaration.packageId, "."))
        error(declaration.location, "declaration has invalid Package ID '" +
                                    declaration.packageId + "'");
    if (!validSeparatedName(declaration.modulePath, "::", true))
        error(declaration.location, "declaration has invalid module path '" +
                                    declaration.modulePath + "'");
    if (auto* function = dynamic_cast<const FunctionDecl*>(&declaration)) {
        verifyFunction(*function, module);
        return;
    }
    if (auto* fragment = dynamic_cast<const FragmentDecl*>(&declaration)) {
        if (!fragment->body)
            error(fragment->location, "fragment '" + fragment->name + "' has no body");
        verifyType(fragment->structuralType, fragment->location,
                   "fragment '" + fragment->name + "' structural type", module);
        for (const auto& parameter : fragment->params)
            verifyType(parameter.type, fragment->location,
                       "fragment parameter '" + parameter.name + "'", module);
        verifyBlock(fragment->body.get(), module, fragment->name);
        return;
    }
    if (auto* structure = dynamic_cast<const StructDecl*>(&declaration)) {
        verifyType(structure->type, structure->location,
                   "struct '" + structure->name + "'", module,
                   !structure->typeParams.empty());
        for (const auto& field : structure->fields)
            verifyType(field.type, structure->location,
                       "field '" + structure->name + "." + field.name + "'",
                       module,
                       !structure->typeParams.empty());
        return;
    }
    if (auto* enumeration = dynamic_cast<const EnumDecl*>(&declaration)) {
        verifyType(enumeration->type, enumeration->location,
                   "enum '" + enumeration->name + "'", module,
                   !enumeration->typeParams.empty());
        return;
    }
    if (auto* trait = dynamic_cast<const TraitDecl*>(&declaration)) {
        verifyType(trait->type, trait->location,
                   "trait '" + trait->name + "'", module, true);
        for (const auto& method : trait->methods) {
            for (const auto& parameter : method.params)
                verifyType(parameter.type, trait->location,
                           "trait method parameter '" + method.name + "." +
                               parameter.name + "'", module, true);
            verifyType(method.returnType, trait->location,
                       "trait method return '" + method.name + "'", module, true);
        }
        return;
    }
    if (auto* implementation = dynamic_cast<const ImplDecl*>(&declaration)) {
        if (implementation->resolvedTraitId.empty())
            error(implementation->location, "implementation has no resolved trait id");
        if (implementation->resolvedTargetTypeId.empty())
            error(implementation->location, "implementation has no resolved target type id");
        verifyType(implementation->targetType, implementation->location,
                   "implementation target", module,
                   !implementation->typeParams.empty());
        for (const auto& method : implementation->methods) {
            if (!method) error(implementation->location, "implementation contains a null method");
            else verifyFunction(
                *method, module,
                !implementation->typeParams.empty());
        }
    }
}

void Verifier::verifyFunction(
    const FunctionDecl& function, const Module& module,
    bool inheritsTypeParameters) {
    const bool generic = isGeneric(function) || inheritsTypeParameters;
    const bool previousAllowance = mAllowTypeParameters;
    mAllowTypeParameters = generic;
    if (function.name.empty()) error(function.location, "function has no source name");
    if (function.generatedSymbolName.empty() && function.linkName.empty())
        error(function.location, "function '" + function.name + "' has no linkage identity");
    for (const auto& parameter : function.params) {
        verifyType(parameter.type, function.location,
                   "parameter '" + function.name + "." + parameter.name + "'",
                   module, generic);
        if (parameter.isLinear !=
            (parameter.usage == luna::ownership::Usage::Linear))
            error(function.location, "parameter '" + function.name + "." +
                  parameter.name + "' has inconsistent linear compatibility flag");
        if (parameter.relation != luna::ownership::Relation::Owned &&
            parameter.usage != luna::ownership::Usage::Copy)
            error(function.location, "borrowed parameter '" + function.name + "." +
                  parameter.name + "' must use copy cardinality");
    }
    verifyType(function.returnType, function.location,
               "return type of '" + function.name + "'", module, generic);
    if (!function.isExtern && !function.body)
        error(function.location, "function '" + function.name + "' has no body");
    if (function.returnsLinear !=
        (function.returnUsage == luna::ownership::Usage::Linear))
        error(function.location, "function '" + function.name +
              "' has inconsistent linear return compatibility flag");
    if (function.isKernel && function.isCodegenReachable && !module.features.kernel)
        error(function.location, "kernel '" + function.name +
                                 "' is present without the kernel feature");
    const auto* returnType = module.findType(function.returnType);
    if (function.isKernel && returnType && returnType->kind != TypeKind::Unit)
        error(function.location, "kernel '" + function.name + "' must return unit");
    if (function.body) verifyBlock(function.body.get(), module, function.name);
    mAllowTypeParameters = previousAllowance;
}

void Verifier::verifyBlock(const BlockStmt* block, const Module& module,
                           const std::string& owner) {
    if (!block) {
        error({}, "null block in '" + owner + "'");
        return;
    }
    for (const auto& statement : block->stmts)
        verifyStmt(statement.get(), module, owner);
}

void Verifier::verifyStmt(const Stmt* stmt, const Module& module,
                          const std::string& owner) {
    if (!stmt) {
        error({}, "null statement in '" + owner + "'");
        return;
    }
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        verifyBlock(block, module, owner);
    } else if (auto* let = dynamic_cast<const LetStmt*>(stmt)) {
        if (let->name.empty()) error(let->location, "binding has no name in '" + owner + "'");
        verifyType(let->type, let->location, "binding '" + let->name + "'", module);
        verifyExpr(let->initializer.get(), module, owner);
        const auto* bindingType = module.findType(let->type);
        auto requiredUsage = bindingType
            ? bindingType->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        if (auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get())) {
            const auto callUsage = call->returnsLinear
                ? luna::ownership::Usage::Linear
                : call->returnUsage;
            requiredUsage = luna::ownership::strongerUsage(
                requiredUsage, callUsage);
        }
        if (!luna::ownership::satisfiesUsageRequirement(
                let->usage, requiredUsage))
            error(let->location, "binding '" + let->name +
                  "' weakens its required usage contract in '" + owner + "'");
        if (let->isLinear != (let->usage == luna::ownership::Usage::Linear))
            error(let->location, "binding '" + let->name +
                  "' has inconsistent linear compatibility flag");
        if (let->materializesIteratorRecipe) {
            if (!bindingType || bindingType->kind != TypeKind::Iterator)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no iterator type");
            auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get());
            if (!call ||
                call->iteratorOp == IteratorOp::None ||
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count ||
                call->iteratorOp == IteratorOp::Collect)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no adapter recipe");
            if (let->materializedIteratorOwnsSource) {
                verifyType(
                    let->materializedIteratorSourceType,
                    let->location,
                    "materialized iterator source", module);
                const auto* source = module.findType(
                    let->materializedIteratorSourceType);
                const auto* sourceElement = source
                    ? module.findType(source->innerTypeId) : nullptr;
                if (!source ||
                    source->kind != TypeKind::Array ||
                    !sourceElement ||
                    sourceElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Copy ||
                    luna::ownership::mustConsume(
                        source->sysmeta.resource.usage))
                    error(let->location,
                          "owning materialized iterator '" +
                          let->name +
                          "' has no affine move-only array source");
            } else if (!let->materializedIteratorSourceType.empty()) {
                error(let->location,
                      "non-owning materialized iterator '" +
                      let->name +
                      "' carries an owning source witness");
            }
        } else if (let->materializedIteratorOwnsSource ||
                   !let->materializedIteratorSourceType.empty()) {
            error(let->location,
                  "ordinary binding '" + let->name +
                  "' carries materialized iterator source state");
        }
    } else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        if (ret->value) verifyExpr(ret->value.get(), module, owner);
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : ret->cleanups) {
            if (cleanup.place.empty())
                error(ret->location, "return cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(ret->location, "duplicate return cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(ret->location, "return cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, ret->location,
                    "return cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        verifyExpr(expression->expr.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyBlock(conditional->thenBlock.get(), module, owner);
        if (conditional->elseBranch)
            verifyStmt(conditional->elseBranch.get(), module, owner);
    } else if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        verifyExpr(match->scrutinee.get(), module, owner);
        verifyType(match->matchedType, match->location, "match type", module);
        if (match->arms.empty())
            error(match->location, "match in '" + owner + "' has no arms");
        const auto* matchedType = module.findType(match->matchedType);
        const size_t expectedVariantCount =
            matchedType && matchedType->kind == TypeKind::Result
                ? 2
                : (matchedType ? matchedType->variants.size() : 0);
        if (match->arms.size() != expectedVariantCount)
            error(match->location, "match in '" + owner +
                  "' is not exhaustive in frozen MoonIR");
        std::unordered_set<uint32_t> variants;
        for (const auto& arm : match->arms) {
            if (!variants.insert(arm.variantIndex).second)
                error(arm.location, "duplicate match variant index in '" +
                      owner + "'");
            if (arm.variantIndex >= expectedVariantCount)
                error(arm.location, "match arm has an out-of-range variant "
                      "index in '" + owner + "'");
            if (arm.bindings.size() != arm.bindingTypes.size())
                error(arm.location, "match arm binding/type arity mismatch in '" +
                      owner + "'");
            if (arm.bindings.size() != arm.bindingUsages.size())
                error(arm.location, "match arm binding/usage arity mismatch in '" +
                      owner + "'");
            TypeRefVec expectedFields;
            if (matchedType && matchedType->kind == TypeKind::Enum &&
                arm.variantIndex <
                    matchedType->variants.size()) {
                expectedFields =
                    matchedType->variants[
                        arm.variantIndex].fields;
            } else if (matchedType &&
                       matchedType->kind ==
                           TypeKind::Result &&
                       matchedType->typeArgumentIds.size() == 2 &&
                       arm.variantIndex < 2) {
                expectedFields.push_back(
                    matchedType->typeArgumentIds[
                        arm.variantIndex == 1 ? 0 : 1]);
            }
            if (arm.bindingTypes.size() != expectedFields.size())
                error(arm.location, "match arm payload arity disagrees with "
                      "its frozen variant in '" + owner + "'");
            const size_t comparable = std::min(
                arm.bindingTypes.size(), expectedFields.size());
            for (size_t index = 0; index < comparable; ++index) {
                if (arm.bindingTypes[index] != expectedFields[index])
                    error(arm.location, "match binding type disagrees with "
                          "its frozen variant payload in '" + owner + "'");
                if (index < arm.bindingUsages.size() &&
                    !luna::ownership::satisfiesUsageRequirement(
                        arm.bindingUsages[index],
                        frozenUsage(module, arm.bindingTypes[index])))
                    error(arm.location, "match binding weakens its required "
                          "usage contract in '" + owner + "'");
            }
            for (const auto& type : arm.bindingTypes)
                verifyType(type, arm.location, "match binding type", module);
            verifyBlock(arm.body.get(), module, owner);
        }
    } else if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        verifyExpr(loop->cond.get(), module, owner);
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        verifyExpr(loop->iterable.get(), module, owner);
        verifyType(loop->elementType, loop->location,
                   "for-loop element type", module);
        if (!luna::ownership::satisfiesUsageRequirement(
                loop->bindingUsage,
                frozenUsage(module, loop->elementType)))
            error(loop->location, "for-loop binding weakens its required "
                  "usage contract in '" + owner + "'");
        if (!loop->protocolNextSymbol.empty()) {
            verifyType(loop->protocolIteratorType, loop->location,
                       "iterator protocol state type", module);
            verifyType(loop->protocolOptionType, loop->location,
                       "iterator protocol option type", module);
            const auto* optionType = module.findType(loop->protocolOptionType);
            if (!optionType || optionType->kind != TypeKind::Enum) {
                error(loop->location,
                      "iterator protocol for-loop requires an enum Option type");
            } else {
                const auto variantCount =
                    optionType->variants.size();
                if (loop->protocolNoneVariant >= variantCount ||
                    loop->protocolSomeVariant >= variantCount ||
                    loop->protocolNoneVariant ==
                        loop->protocolSomeVariant)
                    error(loop->location,
                          "iterator protocol for-loop has invalid Option variants");
            }
            if (!loop->protocolIntoSymbol.empty()) {
                verifyType(loop->protocolInputType,
                           loop->location,
                           "IntoIterator protocol input type", module);
                if (loop->protocolStateName.empty())
                    error(loop->location,
                          "IntoIterator protocol for-loop has no hidden "
                          "state identity");
            } else if (!loop->protocolStateName.empty()) {
                error(loop->location,
                      "direct Iterator for-loop unexpectedly owns a "
                      "hidden state identity");
            }
        }
        if (!loop->recipeStateName.empty()) {
            verifyType(loop->recipeSourceType,
                       loop->location,
                       "consuming recipe source type", module);
            const auto* recipeSource = module.findType(loop->recipeSourceType);
            const auto* recipeElement = recipeSource
                ? module.findType(recipeSource->innerTypeId) : nullptr;
            if (!recipeSource ||
                recipeSource->kind !=
                    TypeKind::Array ||
                !recipeElement ||
                recipeElement->sysmeta.resource.usage ==
                    luna::ownership::Usage::Copy)
                error(loop->location,
                      "consuming recipe state must own a move-only array");
        }
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* release = dynamic_cast<const FreeStmt*>(stmt)) {
        verifyExpr(release->operand.get(), module, owner);
        if (release->operand && !release->operand->type.empty())
            verifyCleanupAction(
                release->action,
                release->operand->type,
                release->location, "free operation", module);
    } else if (auto* slot = dynamic_cast<const SlotInvokeStmt*>(stmt)) {
        for (const auto& argument : slot->args)
            verifyExpr(argument.get(), module, owner);
        verifyBlock(slot->continuation.get(), module, owner);
        if (slot->usesDynamicDispatch && !module.features.dynamicApply)
            error(slot->location, "dynamic slot dispatch is present without dynamic apply capability");
    } else if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt)) {
        if (apply->isDynamic && !module.features.dynamicApply)
            error(apply->location, "dynamic apply is present without dynamic apply capability");
        if (apply->body) verifyBlock(apply->body.get(), module, owner);
    } else if (auto* abort = dynamic_cast<const AbortStmt*>(stmt)) {
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : abort->cleanups) {
            if (cleanup.place.empty())
                error(abort->location, "abort cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(abort->location, "duplicate abort cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(abort->location, "abort cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, abort->location,
                    "abort cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* await = dynamic_cast<const AwaitStmt*>(stmt)) {
        verifyExpr(await->event.get(), module, owner);
    }
}

void Verifier::verifyExpr(const Expr* expr, const Module& module,
                          const std::string& owner) {
    if (!expr) {
        error({}, "null expression in '" + owner + "'");
        return;
    }
    if (auto* selection = dynamic_cast<const DynamicSelectExpr*>(expr)) {
        if (!module.features.dynamicSelect || !module.features.runtime)
            error(selection->location,
                  "dynamic select expression is present without runtime dynamic-select capability");
        verifyType(selection->type, selection->location,
                   "dynamic select callable type", module);
        const auto* selectionType = module.findType(selection->type);
        if (!selectionType || selectionType->kind != TypeKind::Function)
            error(selection->location, "dynamic select must produce a callable type");
        if (selection->familyId.empty() || selection->selectorDeclarationId.empty() ||
            selection->metadataSchemaId.empty())
            error(selection->location, "dynamic select has an incomplete binding identity");
        if (selection->filterArguments.empty() || selection->candidates.empty())
            error(selection->location, "dynamic select has an empty filter or candidate set");
        for (const auto& argument : selection->filterArguments)
            verifyExpr(argument.get(), module, owner);
        std::unordered_set<std::string> candidateIds;
        for (const auto& candidate : selection->candidates) {
            if (candidate.declarationId.empty() || candidate.linkageName.empty())
                error(selection->location, "dynamic select candidate has no stable identity");
            else if (!candidateIds.insert(candidate.declarationId).second)
                error(selection->location, "dynamic select contains duplicate candidate '" +
                                           candidate.declarationId + "'");
            if (candidate.metadataValues.size() != selection->filterArguments.size())
                error(selection->location, "dynamic select candidate metadata shape mismatch");
            const DeclarationRecord* record = nullptr;
            for (const auto& item : module.declarationTable) {
                if (item.id == candidate.declarationId) {
                    record = &item;
                    break;
                }
            }
            if (!record || record->retention == Retention::CompileTime) {
                error(selection->location, "dynamic select candidate '" +
                      candidate.declarationId + "' has no runtime descriptor");
                continue;
            }
            if (record->linkageName != candidate.linkageName ||
                record->familyId != selection->familyId)
                error(selection->location, "dynamic select candidate '" +
                                           candidate.declarationId +
                                           "' does not match its declared linkage/family");
            bool retainedMetadata = false;
            for (const auto& metadata : record->metadata) {
                if (metadata.schemaId == selection->metadataSchemaId &&
                    metadata.retention != Retention::CompileTime &&
                    metadata.values == candidate.metadataValues) {
                    retainedMetadata = true;
                    break;
                }
            }
            if (!retainedMetadata)
                error(selection->location, "dynamic select candidate '" +
                      candidate.declarationId +
                      "' does not retain the inspected metadata at runtime");
        }
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        verifyExpr(binary->lhs.get(), module, owner);
        verifyExpr(binary->rhs.get(), module, owner);
    } else if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        verifyExpr(unary->operand.get(), module, owner);
    } else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
        verifyExpr(call->callee.get(), module, owner);
        for (const auto& argument : call->args)
            verifyExpr(argument.get(), module, owner);
        if (!call->intrinsicType.empty())
            verifyType(call->intrinsicType, call->location,
                       "intrinsic call type witness", module);
        if (call->iteratorOp != IteratorOp::None) {
            verifyType(call->iteratorInputType, call->location,
                       "iterator operation input", module);
            verifyType(call->iteratorOutputType, call->location,
                       "iterator operation output", module);
            const auto* callType = module.findType(call->type);
            if (!callType)
                error(call->location,
                      "iterator operation has no result type");
            const bool terminal =
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count ||
                call->iteratorOp == IteratorOp::Collect;
            if (!terminal &&
                (!callType || callType->kind != TypeKind::Iterator))
                error(call->location,
                      "iterator adapter does not produce an iterator recipe");
            if (call->iteratorOp == IteratorOp::Collect) {
                verifyType(
                    call->iteratorCollectTargetType,
                    call->location,
                    "iterator collect target", module);
                verifyType(
                    call->iteratorCollectBuilderType,
                    call->location,
                    "iterator collect builder", module);
                if (call->iteratorCollectBeginSymbol.empty() ||
                    call->iteratorCollectPushSymbol.empty() ||
                    call->iteratorCollectFinishSymbol.empty())
                    error(call->location,
                          "iterator collect has an incomplete "
                          "FromIterator protocol witness");
            }
            if (!call->iteratorRecipeStateName.empty()) {
                if (!terminal)
                    error(call->location,
                          "non-terminal iterator adapter owns terminal recipe state");
                verifyType(
                    call->iteratorRecipeSourceType,
                    call->location,
                    "iterator terminal recipe source", module);
                const auto* recipeSource = module.findType(
                    call->iteratorRecipeSourceType);
                const auto* recipeElement = recipeSource
                    ? module.findType(recipeSource->innerTypeId) : nullptr;
                if (!recipeSource || recipeSource->kind !=
                        TypeKind::Array ||
                    !recipeElement ||
                    recipeElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Copy)
                    error(call->location,
                          "iterator terminal recipe state does not own a move-only array");
            }
        }
        if (call->returnsLinear !=
            (call->returnUsage == luna::ownership::Usage::Linear))
            error(call->location, "call in '" + owner +
                  "' has inconsistent linear result compatibility flag");
    } else if (auto* launch = dynamic_cast<const LaunchExpr*>(expr)) {
        if (!module.features.kernel)
            error(launch->location, "kernel launch is present without kernel capability");
        if (launch->resolvedKernelName.empty())
            error(launch->location, "kernel launch has no resolved declaration identity");
        verifyExpr(launch->threads.get(), module, owner);
        for (const auto& argument : launch->args)
            verifyExpr(argument.get(), module, owner);
    } else if (auto* variant = dynamic_cast<const VariantConstructExpr*>(expr)) {
        verifyType(variant->constructedType, variant->location,
                   "constructed enum '" + variant->typeName + "'", module);
        for (const auto& argument : variant->args)
            verifyExpr(argument.get(), module, owner);
    } else if (auto* field = dynamic_cast<const FieldAccessExpr*>(expr)) {
        verifyExpr(field->object.get(), module, owner);
    } else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        verifyExpr(index->object.get(), module, owner);
        verifyExpr(index->index.get(), module, owner);
    } else if (auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        verifyType(array->elementType, array->location, "array element", module);
        for (const auto& element : array->elements)
            verifyExpr(element.get(), module, owner);
    } else if (auto* record = dynamic_cast<const RecordLiteralExpr*>(expr)) {
        verifyType(record->type, record->location, "record literal", module);
        const auto* recordType = module.findType(record->type);
        if (!recordType ||
            (recordType->kind != TypeKind::Record &&
             recordType->kind != TypeKind::Struct))
            error(record->location,
                  "record literal has neither a structural record nor named struct type");
        std::unordered_set<std::string> names;
        for (const auto& field : record->fields) {
            if (field.name.empty() || !names.insert(field.name).second)
                error(record->location,
                      "record literal contains an empty or duplicate field");
            verifyExpr(field.value.get(), module, owner);
        }
    } else if (auto* allocation = dynamic_cast<const HeapAllocExpr*>(expr)) {
        verifyType(allocation->allocatedType, allocation->location,
                   "heap allocation", module);
        verifyExpr(allocation->initializer.get(), module, owner);
    } else if (auto* propagation = dynamic_cast<const TryExpr*>(expr)) {
        verifyExpr(propagation->operand.get(), module, owner);
        verifyType(propagation->resultType, propagation->location,
                   "error propagation Result", module);
        verifyType(propagation->propagatedResultType, propagation->location,
                   "propagated Result", module);
        verifyType(propagation->valueType, propagation->location,
                   "error propagation value", module);
        verifyType(propagation->errorType, propagation->location,
                   "error propagation error", module);
        verifyType(propagation->propagatedErrorType, propagation->location,
                   "propagated error", module);
        const auto* resultType = module.findType(propagation->resultType);
        const auto* propagatedResultType = module.findType(
            propagation->propagatedResultType);
        if (!resultType || resultType->kind != TypeKind::Result ||
            resultType->typeArgumentIds.size() != 2)
            error(propagation->location,
                  "error propagation has no validated Result<T, E> type");
        if (!propagatedResultType ||
            propagatedResultType->kind != TypeKind::Result ||
            propagatedResultType->typeArgumentIds.size() != 2)
            error(propagation->location,
                  "error propagation has no validated enclosing Result<T, E> type");
        if (propagation->errorType != propagation->propagatedErrorType &&
            propagation->errorConversionSymbol.empty())
            error(propagation->location,
                  "error propagation changes error type without a static From conversion");
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : propagation->cleanups) {
            if (cleanup.place.empty())
                error(propagation->location,
                      "error propagation cleanup has no place in '" + owner + "'");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(propagation->location,
                      "duplicate error propagation cleanup for '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(propagation->location,
                      "error propagation cleanup for '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId,
                    propagation->location,
                    "error propagation cleanup for '" + cleanup.place + "'",
                    module);
        }
    } else if (auto* move = dynamic_cast<const MoveExpr*>(expr)) {
        verifyExpr(move->operand.get(), module, owner);
    } else if (auto* borrow = dynamic_cast<const BorrowExpr*>(expr)) {
        verifyExpr(borrow->operand.get(), module, owner);
    } else if (auto* dereference = dynamic_cast<const DerefExpr*>(expr)) {
        verifyExpr(dereference->operand.get(), module, owner);
    } else if (auto* address = dynamic_cast<const AddrOfExpr*>(expr)) {
        verifyExpr(address->operand.get(), module, owner);
    } else if (auto* block = dynamic_cast<const BlockExpr*>(expr)) {
        verifyBlock(block->block.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfExpr*>(expr)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyExpr(conditional->thenExpr.get(), module, owner);
        verifyExpr(conditional->elseExpr.get(), module, owner);
    } else if (auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
        verifyType(lambda->returnType, lambda->location,
                   "lambda return type", module);
        verifyBlock(lambda->body.get(), module, owner);
    } else if (auto* assignment = dynamic_cast<const AssignExpr*>(expr)) {
        verifyExpr(assignment->lhs.get(), module, owner);
        verifyExpr(assignment->rhs.get(), module, owner);
    }
}

void Verifier::verifyType(const TypeRef& reference,
                          const SourceLocation& location,
                          const std::string& context,
                          const Module& module,
                          bool allowTypeParameter) {
    if (reference.empty()) {
        error(location, context + " has no resolved type");
        return;
    }
    const auto* type = module.findType(reference);
    if (!type) {
        error(location, context + " references type '" + reference.value +
                        "' absent from the MoonIR type table");
        return;
    }
    if (!mActiveTypeIds.insert(reference.value).second) return;
    if (type->domain == luna::types::TypeDomain::Inference ||
        type->domain == luna::types::TypeDomain::Error)
        error(location, context + " contains a non-materialized Sema type");
    if (type->kind == TypeKind::Unknown || type->kind == TypeKind::InferenceVar)
        error(location, context + " contains an unresolved type");
    if (type->kind == TypeKind::TypeParam &&
        !allowTypeParameter && !mAllowTypeParameters)
        error(location, context + " contains a type parameter outside a generic recipe");
    if (!type->innerTypeId.empty())
        verifyType(type->innerTypeId, location, context, module,
                   allowTypeParameter);
    for (const auto& argument : type->typeArgumentIds)
        verifyType(argument, location, context, module, allowTypeParameter);
    for (const auto& parameter : type->parameterTypeIds)
        verifyType(parameter, location, context, module, allowTypeParameter);
    if (!type->returnTypeId.empty())
        verifyType(type->returnTypeId, location, context, module,
                   allowTypeParameter);
    for (const auto& field : type->fields)
        verifyType(field.type, location, context + "." + field.name, module,
                   allowTypeParameter);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields)
            verifyType(field, location, context + "::" + variant.name, module,
                       allowTypeParameter);
    mActiveTypeIds.erase(reference.value);
}

void Verifier::error(const SourceLocation& location, const std::string& message) {
    mErrors.push_back(diagnostic::format(
        "moon-verify", message, location.path, location.line, location.column,
        "MoonIR must be valid before LLVM lowering or Moon container emission"));
}

} // namespace moon
