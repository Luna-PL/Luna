#include "Verifier.h"
#include "VerifierInternal.h"
#include "../core/TypeLayout.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace moon {

using namespace verifier_detail;

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
             type.kind == TypeKind::SymbolSet ||
             type.kind == TypeKind::DeclarationView ||
             type.kind == TypeKind::DeclarationRef) &&
            type.domain != luna::types::TypeDomain::Compiler)
            error({}, "compile-time view/reference type '" + type.id.value +
                      "' is outside the Compiler type domain");
        if (type.kind == TypeKind::SymbolSet)
            error({}, "compiler-only symbol_set type '" + type.id.value +
                      "' was not erased before MoonIR");
        if (type.kind == TypeKind::Enum &&
            type.domain == luna::types::TypeDomain::Compiler &&
            type.nominalDeclarationId == luna::sysmeta::OptionTypeId)
            error({}, "compiler-only Option type '" + type.id.value +
                      "' was not erased before MoonIR");
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
        if (!type.sysmeta.abi.dropGlueSymbol.empty())
            error({}, "type '" + type.id.value +
                      "' retains a linkage-string Drop reference");
        if (type.sysmeta.resource.needsDrop != type.dropGlue.complete())
            error({}, "type '" + type.id.value +
                      "' has inconsistent Drop sysmeta");
        if (!type.dropGlue.empty())
            verifyDeclarationRef(
                type.dropGlue, {}, "Drop glue for type '" +
                type.id.value + "'", module, DeclarationKind::Function);
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
        for (const auto& field : type.capturedFields)
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
        std::string domainError;
        if (!luna::types::isWellFormedTypeDomain(
                restored, &domainError))
            error({}, "frozen type domain is invalid: " + domainError);
        if (!luna::layout::valueLayoutFits(restored))
            error({}, "frozen type '" + type.id.value +
                      "' exceeds the 64-bit ABI size limit");
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
        if (!record.sysmeta.abi.dropGlueSymbol.empty())
            error(record.location, "declaration '" + record.id +
                                   "' retains a linkage-string Drop reference");
        if (!record.dropGlue.empty())
            verifyDeclarationRef(
                record.dropGlue, record.location,
                "Drop glue for declaration '" + record.id + "'",
                module, DeclarationKind::Function);
        if (record.sysmeta.capability.runtimeRetained !=
            (record.retention != Retention::CompileTime))
            error(record.location, "declaration '" + record.id +
                                   "' has inconsistent runtime-retention sysmeta");
        if (!record.type.empty()) {
            const auto* recordType = module.findType(record.type);
            if (!recordType)
                error(record.location, "declaration '" + record.id +
                                       "' references a type absent from the type table");
            else if (record.kind == DeclarationKind::Slot &&
                     recordType->kind != TypeKind::Slot)
                error(record.location, "slot declaration '" + record.id +
                                       "' does not reference a slot type");
            else if (record.kind == DeclarationKind::Fragment &&
                     recordType->kind != TypeKind::Fragment)
                error(record.location, "fragment declaration '" + record.id +
                                       "' does not reference a fragment type");
            else if ((recordType->kind == TypeKind::Function ||
                      recordType->kind == TypeKind::Slot ||
                      recordType->kind == TypeKind::Fragment) &&
                record.sysmeta.resource.parameters.size() !=
                    recordType->parameterTypeIds.size())
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta parameter contract count does not match its type");
            if (recordType && record.dropGlue != recordType->dropGlue)
                error(record.location, "declaration '" + record.id +
                                       "' Drop reference differs from its frozen type");
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
        if (record.kind == DeclarationKind::Fragment ||
            record.kind == DeclarationKind::Slot) {
            const auto form = record.sysmeta.control.form;
            if (form != luna::sysmeta::ControlForm::Interceptor &&
                form != luna::sysmeta::ControlForm::Context)
                error(record.location, "control declaration '" + record.id +
                                       "' has no slot/fragment control sysmeta");
            if (record.sysmeta.control.storage !=
                luna::sysmeta::ContinuationStorage::ScopedStack)
                error(record.location, "control declaration '" + record.id +
                                       "' must use a scoped continuation in the current ABI");
            if (!record.sysmeta.capability.hostOnly)
                error(record.location, "control declaration '" + record.id +
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

    std::string previousImportKey;
    std::unordered_map<std::string, std::string> hostLinkContracts;
    for (const auto& import : module.imports) {
        const std::string key =
            std::to_string(static_cast<unsigned>(import.kind)) + "\n" +
            import.ownerPackageId + "\n" + import.localName + "\n" +
            import.packageId + "\n" + import.alias;
        if (!previousImportKey.empty() && key <= previousImportKey)
            error(import.location, "import table is duplicate or out of canonical order");
        previousImportKey = key;
        if (!validSeparatedName(import.ownerPackageId, "."))
            error(import.location, "import has invalid owner Package ID '" +
                                   import.ownerPackageId + "'");
        if (import.kind == ImportKind::Package) {
            if (!validSeparatedName(import.packageId, ".") ||
                !validIdentifier(import.alias))
                error(import.location, "package import has an invalid package or alias");
            if (!import.localName.empty() || !import.capabilityId.empty() ||
                !import.linkSymbol.empty() || !import.abi.empty() ||
                !import.declaration.empty() || !import.type.empty())
                error(import.location, "package import carries host-only fields");
        } else if (import.kind == ImportKind::Host) {
            if (!validSeparatedName(import.localName, "::") ||
                !validSeparatedName(import.capabilityId, ".") ||
                import.linkSymbol.empty() || import.abi != "C" ||
                !import.declaration.complete() || import.type.empty()) {
                error(import.location, "host import has incomplete typed capability fields");
                continue;
            }
            if (!import.packageId.empty() || !import.alias.empty())
                error(import.location, "host import carries package-only fields");
            const auto* declaration = module.findDeclaration(import.declaration);
            if (!declaration || declaration->kind != DeclarationKind::Function)
                error(import.location, "host import references a missing function contract");
            else if (declaration->type != import.type)
                error(import.location, "host import type differs from its declaration contract");
            auto [found, inserted] = hostLinkContracts.emplace(
                import.linkSymbol, import.declaration.contract.value);
            if (!inserted && found->second != import.declaration.contract.value)
                error(import.location, "host imports reuse link symbol '" +
                                       import.linkSymbol + "' with different contracts");
        } else {
            error(import.location, "import table contains an invalid kind");
        }
    }

    std::string previousExportKey;
    std::unordered_set<std::string> exportNames;
    for (const auto& exported : module.exports) {
        const std::string key = exported.name + "\n" +
            exported.declaration.symbol.value;
        if (!previousExportKey.empty() && key <= previousExportKey)
            error(exported.location, "export table is duplicate or out of canonical order");
        previousExportKey = key;
        if (exported.name.empty() || !exportNames.insert(exported.name).second ||
            !exported.declaration.complete() || exported.type.empty()) {
            error(exported.location, "export has incomplete or duplicate public identity");
            continue;
        }
        const auto* declaration = module.findDeclaration(exported.declaration);
        if (!declaration)
            error(exported.location, "export references a missing declaration contract");
        else if (declaration->type != exported.type ||
                 declaration->kind != exported.kind)
            error(exported.location, "export type or kind differs from its declaration");
        if (!exported.abi.empty() && exported.abi != "C")
            error(exported.location, "export carries an unsupported explicit ABI");
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
            if (declaration->contractId != record->second->contractId)
                error(declaration->location, "declaration table ContractId mismatch for '" +
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

    if (module.features.kernelRuntimeReserved && !module.features.kernel)
        error({}, "reserved kernel runtime requires the kernel feature");

    return mErrors.empty();
}

} // namespace moon
