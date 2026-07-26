#include "Verifier.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace moon {

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

bool fitsInitialResultPayload(TypeKind kind) {
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
        case TypeKind::F32:
        case TypeKind::F64:
        case TypeKind::Bool:
        case TypeKind::String:
        case TypeKind::CStr:
        case TypeKind::RawPointer:
        case TypeKind::Unit:
        case TypeKind::Struct:
        case TypeKind::Record:
        case TypeKind::Enum:
        case TypeKind::TypeParam:
        case TypeKind::Reference:
        case TypeKind::Rc:
        case TypeKind::Arc:
        case TypeKind::Function:
        case TypeKind::DeviceBuffer:
        case TypeKind::Event:
            return true;
        default:
            return false;
    }
}

bool metadataConstantMatches(const ConstantValue& value, const TypePtr& type) {
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
        if (type.kind == TypeKind::Rc &&
            type.sysmeta.resource.management !=
                luna::sysmeta::ResourceManagement::Rc)
            error({}, "rc type '" + type.id.value +
                      "' has inconsistent resource sysmeta");
        if (type.kind == TypeKind::Arc &&
            type.sysmeta.resource.management !=
                luna::sysmeta::ResourceManagement::Arc)
            error({}, "arc type '" + type.id.value +
                      "' has inconsistent resource sysmeta");
        if (type.kind == TypeKind::Result) {
            if (type.referencedTypeIds.size() != 2) {
                error({}, "Result type '" + type.id.value +
                          "' must reference exactly value and error payload types");
            } else {
                for (const auto& payloadId : type.referencedTypeIds) {
                    const auto* payload = module.findType(payloadId);
                    if (payload && !fitsInitialResultPayload(payload->kind))
                        error({}, "Result payload type '" + payload->displayName +
                                  "' does not fit the initial one-word Result ABI");
                }
            }
        }
        if (type.sysmeta.resource.needsDrop !=
            !type.sysmeta.abi.dropGlueSymbol.empty())
            error({}, "type '" + type.id.value +
                      "' has inconsistent Drop sysmeta");
    }
    for (const auto& type : module.typeTable) {
        for (const auto& referenced : type.referencedTypeIds) {
            if (!module.findType(referenced))
                error({}, "frozen type '" + type.id.value +
                      "' references missing type '" + referenced.value + "'");
        }
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
                       "metadata field '" + schema.name + "." + field.name + "'");
        }
    }

    std::unordered_set<std::string> declarationIds;
    std::unordered_set<std::string> linkageNames;
    std::unordered_map<std::string, const DeclarationRecord*> recordsById;
    for (const auto& record : module.declarationTable) {
        if (record.id.empty()) error(record.location, "declaration table entry has no id");
        else if (!declarationIds.insert(record.id).second) {
            error(record.location, "duplicate declaration id '" + record.id + "'");
        } else {
            recordsById.emplace(record.id, &record);
        }
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
        if (record.type) {
            const auto stableType = luna::types::typeId(record.type);
            if (!mVerifiedTypeIds.count(stableType.value))
                error(record.location, "declaration '" + record.id +
                                       "' references a type absent from the type table");
            if ((record.type->kind == TypeKind::Function ||
                 record.type->kind == TypeKind::Slot ||
                 record.type->kind == TypeKind::Fragment) &&
                record.sysmeta.resource.parameters.size() !=
                    record.type->paramTypes.size())
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta parameter contract count does not match its type");
            const size_t contractCount = std::min(
                record.sysmeta.resource.parameters.size(),
                record.type->paramContracts.size());
            for (size_t index = 0; index < contractCount; ++index) {
                if (record.sysmeta.resource.parameters[index] !=
                    record.type->paramContracts[index])
                    error(record.location, "declaration '" + record.id +
                                           "' sysmeta ownership contract differs from its type");
            }
            if (record.sysmeta.resource.result != record.type->returnContract)
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
                    if (!metadataConstantMatches(metadata.values[index], fields[index].type))
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
                   "fragment '" + fragment->name + "' structural type");
        for (const auto& parameter : fragment->params)
            verifyType(parameter.type, fragment->location,
                       "fragment parameter '" + parameter.name + "'");
        verifyBlock(fragment->body.get(), module, fragment->name);
        return;
    }
    if (auto* structure = dynamic_cast<const StructDecl*>(&declaration)) {
        verifyType(structure->type, structure->location,
                   "struct '" + structure->name + "'", !structure->typeParams.empty());
        for (const auto& field : structure->fields)
            verifyType(field.type, structure->location,
                       "field '" + structure->name + "." + field.name + "'",
                       !structure->typeParams.empty());
        return;
    }
    if (auto* enumeration = dynamic_cast<const EnumDecl*>(&declaration)) {
        verifyType(enumeration->type, enumeration->location,
                   "enum '" + enumeration->name + "'", !enumeration->typeParams.empty());
        return;
    }
    if (auto* trait = dynamic_cast<const TraitDecl*>(&declaration)) {
        verifyType(trait->type, trait->location,
                   "trait '" + trait->name + "'", true);
        for (const auto& method : trait->methods) {
            for (const auto& parameter : method.params)
                verifyType(parameter.type, trait->location,
                           "trait method parameter '" + method.name + "." +
                               parameter.name + "'", true);
            verifyType(method.returnType, trait->location,
                       "trait method return '" + method.name + "'", true);
        }
        return;
    }
    if (auto* implementation = dynamic_cast<const ImplDecl*>(&declaration)) {
        if (implementation->resolvedTraitId.empty())
            error(implementation->location, "implementation has no resolved trait id");
        if (implementation->resolvedTargetTypeId.empty())
            error(implementation->location, "implementation has no resolved target type id");
        verifyType(implementation->targetType, implementation->location,
                   "implementation target", !implementation->typeParams.empty());
        for (const auto& method : implementation->methods) {
            if (!method) error(implementation->location, "implementation contains a null method");
            else verifyFunction(*method, module);
        }
    }
}

void Verifier::verifyFunction(const FunctionDecl& function, const Module& module) {
    const bool generic = isGeneric(function);
    if (function.name.empty()) error(function.location, "function has no source name");
    if (function.generatedSymbolName.empty() && function.linkName.empty())
        error(function.location, "function '" + function.name + "' has no linkage identity");
    for (const auto& parameter : function.params) {
        verifyType(parameter.type, function.location,
                   "parameter '" + function.name + "." + parameter.name + "'", generic);
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
               "return type of '" + function.name + "'", generic);
    if (!function.isExtern && !function.body)
        error(function.location, "function '" + function.name + "' has no body");
    if (function.returnsLinear !=
        (function.returnUsage == luna::ownership::Usage::Linear))
        error(function.location, "function '" + function.name +
              "' has inconsistent linear return compatibility flag");
    if (function.isKernel && function.isCodegenReachable && !module.features.kernel)
        error(function.location, "kernel '" + function.name +
                                 "' is present without the kernel feature");
    if (function.isKernel && function.returnType &&
        function.returnType->kind != TypeKind::Unit)
        error(function.location, "kernel '" + function.name + "' must return unit");
    if (function.body) verifyBlock(function.body.get(), module, function.name);
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
        verifyType(let->type, let->location, "binding '" + let->name + "'");
        verifyExpr(let->initializer.get(), module, owner);
        if (let->isLinear != (let->usage == luna::ownership::Usage::Linear))
            error(let->location, "binding '" + let->name +
                  "' has inconsistent linear compatibility flag");
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
        }
    } else if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        verifyExpr(expression->expr.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyBlock(conditional->thenBlock.get(), module, owner);
        if (conditional->elseBranch)
            verifyStmt(conditional->elseBranch.get(), module, owner);
    } else if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        verifyExpr(loop->cond.get(), module, owner);
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        verifyExpr(loop->iterable.get(), module, owner);
        verifyType(loop->elementType, loop->location,
                   "for-loop element type");
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* release = dynamic_cast<const FreeStmt*>(stmt)) {
        verifyExpr(release->operand.get(), module, owner);
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
        verifyType(selection->type, selection->location, "dynamic select callable type");
        if (!selection->type || selection->type->kind != TypeKind::Function)
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
        if (call->intrinsicType)
            verifyType(call->intrinsicType, call->location,
                       "intrinsic call type witness");
        if (call->iteratorOp != IteratorOp::None) {
            verifyType(call->iteratorInputType, call->location,
                       "iterator operation input");
            verifyType(call->iteratorOutputType, call->location,
                       "iterator operation output");
            if (!call->type)
                error(call->location,
                      "iterator operation has no result type");
            const bool terminal =
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count;
            if (!terminal &&
                (!call->type ||
                 call->type->kind != TypeKind::Iterator))
                error(call->location,
                      "iterator adapter does not produce an iterator recipe");
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
                   "constructed enum '" + variant->typeName + "'");
        for (const auto& argument : variant->args)
            verifyExpr(argument.get(), module, owner);
    } else if (auto* field = dynamic_cast<const FieldAccessExpr*>(expr)) {
        verifyExpr(field->object.get(), module, owner);
    } else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        verifyExpr(index->object.get(), module, owner);
        verifyExpr(index->index.get(), module, owner);
    } else if (auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        verifyType(array->elementType, array->location, "array element");
        for (const auto& element : array->elements)
            verifyExpr(element.get(), module, owner);
    } else if (auto* allocation = dynamic_cast<const HeapAllocExpr*>(expr)) {
        verifyType(allocation->allocatedType, allocation->location, "heap allocation");
        verifyExpr(allocation->initializer.get(), module, owner);
    } else if (auto* propagation = dynamic_cast<const TryExpr*>(expr)) {
        verifyExpr(propagation->operand.get(), module, owner);
        verifyType(propagation->resultType, propagation->location,
                   "error propagation Result");
        verifyType(propagation->valueType, propagation->location,
                   "error propagation value");
        verifyType(propagation->errorType, propagation->location,
                   "error propagation error");
        if (!propagation->resultType ||
            propagation->resultType->kind != TypeKind::Result ||
            propagation->resultType->typeArgs.size() != 2)
            error(propagation->location,
                  "error propagation has no validated Result<T, E> type");
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
        verifyType(lambda->returnType, lambda->location, "lambda return type");
        verifyBlock(lambda->body.get(), module, owner);
    } else if (auto* assignment = dynamic_cast<const AssignExpr*>(expr)) {
        verifyExpr(assignment->lhs.get(), module, owner);
        verifyExpr(assignment->rhs.get(), module, owner);
    }
}

void Verifier::verifyType(const TypePtr& type, const SourceLocation& location,
                          const std::string& context, bool allowTypeParameter) {
    if (!type) {
        error(location, context + " has no resolved type");
        return;
    }
    const auto stableId = luna::types::typeId(type);
    if (!mVerifiedTypeIds.count(stableId.value))
        error(location, context + " references type '" + stableId.value +
                        "' absent from the MoonIR type table");
    if (type->domain == luna::types::TypeDomain::Inference ||
        type->domain == luna::types::TypeDomain::Error)
        error(location, context + " contains a non-materialized Sema type");
    if (type->kind == TypeKind::Unknown || type->kind == TypeKind::InferenceVar)
        error(location, context + " contains an unresolved type");
    if (type->kind == TypeKind::TypeParam && !allowTypeParameter)
        error(location, context + " contains a type parameter outside a generic recipe");
    if (type->inner) verifyType(type->inner, location, context, allowTypeParameter);
    for (const auto& argument : type->typeArgs)
        verifyType(argument, location, context, allowTypeParameter);
    for (const auto& parameter : type->paramTypes)
        verifyType(parameter, location, context, allowTypeParameter);
    if (type->returnType) verifyType(type->returnType, location, context, allowTypeParameter);
    for (const auto& field : type->fields)
        verifyType(field.type, location, context + "." + field.name, allowTypeParameter);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields)
            verifyType(field, location, context + "::" + variant.name, allowTypeParameter);
}

void Verifier::error(const SourceLocation& location, const std::string& message) {
    mErrors.push_back(diagnostic::format(
        "moon-verify", message, location.path, location.line, location.column,
        "MoonIR must be valid before LLVM lowering or Moon container emission"));
}

} // namespace moon
