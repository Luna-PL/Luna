#include "ContainerModelInternal.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace moon {
namespace container_detail {

void encodeIdentity(Encoder& encoder, const luna::sysmeta::IdentityFacts& facts) {
    encoder.string(facts.type.value);
    encoder.string(facts.shape.value);
    encoder.string(facts.symbol.value);
    encoder.string(facts.contract.value);
    encoder.string(facts.abiLayout.value);
}

bool decodeIdentity(Decoder& decoder, luna::sysmeta::IdentityFacts& facts) {
    return decoder.string(facts.type.value) &&
        decoder.string(facts.shape.value) &&
        decoder.string(facts.symbol.value) &&
        decoder.string(facts.contract.value) &&
        decoder.string(facts.abiLayout.value);
}

void encodeContract(Encoder& encoder, const luna::ownership::Contract& contract) {
    encoder.enumeration(contract.relation);
    encoder.enumeration(contract.usage);
}

bool decodeContract(Decoder& decoder, luna::ownership::Contract& contract) {
    return decoder.enumeration(contract.relation, 2) &&
        decoder.enumeration(contract.usage, 2);
}

void encodeFacts(Encoder& encoder, const luna::sysmeta::Facts& facts) {
    encoder.u32(facts.schemaMajor);
    encoder.u32(facts.schemaMinor);
    encodeIdentity(encoder, facts.identity);
    encoder.enumeration(facts.control.form);
    encoder.enumeration(facts.control.cardinality);
    encoder.enumeration(facts.control.storage);
    encoder.enumeration(facts.control.forwarding);
    encoder.boolean(facts.control.abortPermitted);
    encoder.boolean(facts.control.replayValidated);
    encoder.rows(facts.resource.parameters, [&](const auto& contract) {
        encodeContract(encoder, contract);
    });
    encodeContract(encoder, facts.resource.result);
    encoder.enumeration(facts.resource.management);
    encoder.enumeration(facts.resource.releaseDomain);
    encoder.enumeration(facts.resource.lifetime);
    encoder.enumeration(facts.resource.relation);
    encoder.enumeration(facts.resource.usage);
    encoder.enumeration(facts.resource.cleanup);
    encoder.boolean(facts.resource.cleanupRequired);
    encoder.boolean(facts.resource.recursiveCleanup);
    encoder.boolean(facts.resource.needsDrop);
    encoder.boolean(facts.resource.tracksElementInitialization);
    encoder.boolean(facts.capability.hostOnly);
    encoder.boolean(facts.capability.runtimeRetained);
    encoder.boolean(facts.capability.ffi);
    encoder.boolean(facts.capability.gpu);
    encoder.boolean(facts.capability.maySuspend);
    encoder.boolean(facts.abi.stableBoundary);
    encoder.boolean(facts.abi.persistentFrameRequired);
    encoder.string(facts.abi.dropGlueSymbol);
}

bool decodeFacts(Decoder& decoder, luna::sysmeta::Facts& facts) {
    uint32_t schemaMajor = 0;
    uint32_t schemaMinor = 0;
    if (!decoder.u32(schemaMajor) || !decoder.u32(schemaMinor)) return false;
    if (schemaMajor > std::numeric_limits<uint16_t>::max() ||
        schemaMinor > std::numeric_limits<uint16_t>::max())
        return false;
    facts.schemaMajor = static_cast<uint16_t>(schemaMajor);
    facts.schemaMinor = static_cast<uint16_t>(schemaMinor);
    if (!decodeIdentity(decoder, facts.identity) ||
        !decoder.enumeration(facts.control.form, 3) ||
        !decoder.enumeration(facts.control.cardinality, 2) ||
        !decoder.enumeration(facts.control.storage, 2) ||
        !decoder.enumeration(facts.control.forwarding, 2) ||
        !decoder.boolean(facts.control.abortPermitted) ||
        !decoder.boolean(facts.control.replayValidated))
        return false;
    uint32_t parameterCount = 0;
    if (!decoder.rowCount(parameterCount)) return false;
    facts.resource.parameters.clear();
    for (uint32_t index = 0; index < parameterCount; ++index) {
        luna::ownership::Contract contract;
        if (!decodeContract(decoder, contract)) return false;
        facts.resource.parameters.push_back(contract);
    }
    return decodeContract(decoder, facts.resource.result) &&
        decoder.enumeration(facts.resource.management, 1) &&
        decoder.enumeration(facts.resource.releaseDomain, 5) &&
        decoder.enumeration(facts.resource.lifetime, 3) &&
        decoder.enumeration(facts.resource.relation, 2) &&
        decoder.enumeration(facts.resource.usage, 2) &&
        decoder.enumeration(facts.resource.cleanup, 7) &&
        decoder.boolean(facts.resource.cleanupRequired) &&
        decoder.boolean(facts.resource.recursiveCleanup) &&
        decoder.boolean(facts.resource.needsDrop) &&
        decoder.boolean(facts.resource.tracksElementInitialization) &&
        decoder.boolean(facts.capability.hostOnly) &&
        decoder.boolean(facts.capability.runtimeRetained) &&
        decoder.boolean(facts.capability.ffi) &&
        decoder.boolean(facts.capability.gpu) &&
        decoder.boolean(facts.capability.maySuspend) &&
        decoder.boolean(facts.abi.stableBoundary) &&
        decoder.boolean(facts.abi.persistentFrameRequired) &&
        decoder.string(facts.abi.dropGlueSymbol);
}

void encodeReference(Encoder& encoder, const DeclarationRef& reference) {
    encoder.string(reference.symbol.value);
    encoder.string(reference.contract.value);
}

bool decodeReference(Decoder& decoder, DeclarationRef& reference) {
    return decoder.string(reference.symbol.value) &&
        decoder.string(reference.contract.value);
}

void encodeFields(Encoder& encoder, const std::vector<TypeFieldRecord>& fields) {
    encoder.rows(fields, [&](const auto& field) {
        encoder.string(field.name);
        encoder.string(field.type.value);
    });
}

bool decodeFields(Decoder& decoder, std::vector<TypeFieldRecord>& fields) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    fields.clear();
    for (uint32_t index = 0; index < count; ++index) {
        TypeFieldRecord field;
        if (!decoder.string(field.name) || !decoder.string(field.type.value))
            return false;
        fields.push_back(std::move(field));
    }
    return true;
}

void encodeType(Encoder& encoder, const TypeRecord& type) {
    encoder.string(type.id.value);
    encoder.string(type.shapeId.value);
    encoder.string(type.abiLayoutId.value);
    encoder.enumeration(type.domain);
    encoder.enumeration(type.identityMode);
    encoder.enumeration(type.kind);
    encodeFacts(encoder, type.sysmeta);
    encoder.string(type.displayName);
    encoder.string(type.sourceName);
    encoder.string(type.declarationLinkageName);
    encoder.string(type.nominalDeclarationId);
    encoder.rows(type.typeParameterNames, [&](const auto& name) {
        encoder.string(name);
    });
    encodeTypeRefs(encoder, type.typeArgumentIds);
    encoder.string(type.innerTypeId.value);
    encoder.u64(type.arrayLength);
    encoder.boolean(type.isMutable);
    encodeTypeRefs(encoder, type.parameterTypeIds);
    encoder.string(type.returnTypeId.value);
    encoder.rows(type.parameterContracts, [&](const auto& contract) {
        encodeContract(encoder, contract);
    });
    encodeContract(encoder, type.returnContract);
    encoder.boolean(type.isMultiShot);
    encoder.enumeration(type.continuationKind);
    encoder.enumeration(type.iteratorMode);
    encodeFields(encoder, type.fields);
    encodeFields(encoder, type.capturedFields);
    encoder.rows(type.variants, [&](const auto& variant) {
        encoder.string(variant.name);
        encodeTypeRefs(encoder, variant.fields);
    });
    encoder.i64(type.inferenceId);
    encoder.string(type.canonicalType);
    encoder.string(type.canonicalShape);
    encoder.string(type.canonicalAbiLayout);
    encoder.u32(type.layoutAbiVersion);
    encoder.u64(type.valueSize);
    encoder.u64(type.valueAlignment);
    encoder.string(type.abiLayout);
    encodeReference(encoder, type.dropGlue);
    encodeTypeRefs(encoder, type.referencedTypeIds);
}

bool decodeStringRows(Decoder& decoder, std::vector<std::string>& values) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    values.clear();
    for (uint32_t index = 0; index < count; ++index) {
        std::string value;
        if (!decoder.string(value)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

bool decodeType(Decoder& decoder, TypeRecord& type) {
    if (!decoder.string(type.id.value) ||
        !decoder.string(type.shapeId.value) ||
        !decoder.string(type.abiLayoutId.value) ||
        !decoder.enumeration(type.domain, 4) ||
        !decoder.enumeration(type.identityMode, 6) ||
        !decoder.enumeration(type.kind, static_cast<uint32_t>(TypeKind::Unknown)) ||
        !decodeFacts(decoder, type.sysmeta) ||
        !decoder.string(type.displayName) ||
        !decoder.string(type.sourceName) ||
        !decoder.string(type.declarationLinkageName) ||
        !decoder.string(type.nominalDeclarationId) ||
        !decodeStringRows(decoder, type.typeParameterNames) ||
        !decodeTypeRefs(decoder, type.typeArgumentIds) ||
        !decoder.string(type.innerTypeId.value) ||
        !decoder.u64(type.arrayLength) ||
        !decoder.boolean(type.isMutable) ||
        !decodeTypeRefs(decoder, type.parameterTypeIds) ||
        !decoder.string(type.returnTypeId.value))
        return false;

    uint32_t contractCount = 0;
    if (!decoder.rowCount(contractCount)) return false;
    type.parameterContracts.clear();
    for (uint32_t index = 0; index < contractCount; ++index) {
        luna::ownership::Contract contract;
        if (!decodeContract(decoder, contract)) return false;
        type.parameterContracts.push_back(contract);
    }
    if (!decodeContract(decoder, type.returnContract) ||
        !decoder.boolean(type.isMultiShot) ||
        !decoder.enumeration(type.continuationKind, 1) ||
        !decoder.enumeration(type.iteratorMode, 4) ||
        !decodeFields(decoder, type.fields) ||
        !decodeFields(decoder, type.capturedFields))
        return false;

    uint32_t variantCount = 0;
    if (!decoder.rowCount(variantCount)) return false;
    type.variants.clear();
    for (uint32_t index = 0; index < variantCount; ++index) {
        TypeVariantRecord variant;
        if (!decoder.string(variant.name) ||
            !decodeTypeRefs(decoder, variant.fields))
            return false;
        type.variants.push_back(std::move(variant));
    }
    int64_t inferenceId = 0;
    if (!decoder.i64(inferenceId) ||
        inferenceId < std::numeric_limits<int>::min() ||
        inferenceId > std::numeric_limits<int>::max())
        return false;
    type.inferenceId = static_cast<int>(inferenceId);
    return decoder.string(type.canonicalType) &&
        decoder.string(type.canonicalShape) &&
        decoder.string(type.canonicalAbiLayout) &&
        decoder.u32(type.layoutAbiVersion) &&
        decoder.u64(type.valueSize) &&
        decoder.u64(type.valueAlignment) &&
        decoder.string(type.abiLayout) &&
        decodeReference(decoder, type.dropGlue) &&
        decodeTypeRefs(decoder, type.referencedTypeIds);
}

uint32_t featureBits(const FeatureFlags& features) {
    return (features.runtime ? 1u << 0 : 0) |
        (features.kernel ? 1u << 4 : 0) |
        (features.kernelRuntimeReserved ? 1u << 5 : 0);
}

void decodeFeatureBits(uint32_t bits, FeatureFlags& features) {
    features.runtime = (bits & (1u << 0)) != 0;
    features.kernel = (bits & (1u << 4)) != 0;
    features.kernelRuntimeReserved = (bits & (1u << 5)) != 0;
}

void encodeLocation(Encoder& encoder, const SourceLocation& location) {
    encoder.string(location.path);
    encoder.i64(location.line);
    encoder.i64(location.column);
}

bool decodeLocation(Decoder& decoder, SourceLocation& location) {
    int64_t line = 0;
    int64_t column = 0;
    if (!decoder.string(location.path) || !decoder.i64(line) ||
        !decoder.i64(column) || line < std::numeric_limits<int>::min() ||
        line > std::numeric_limits<int>::max() ||
        column < std::numeric_limits<int>::min() ||
        column > std::numeric_limits<int>::max())
        return false;
    location.line = static_cast<int>(line);
    location.column = static_cast<int>(column);
    return true;
}

void encodeConstant(Encoder& encoder, const ConstantValue& value) {
    encoder.u32(static_cast<uint32_t>(value.index()));
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        encoder.i64(*integer);
    } else if (const auto* floating = std::get_if<double>(&value)) {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(*floating));
        std::memcpy(&bits, floating, sizeof(bits));
        encoder.u64(bits);
    } else if (const auto* boolean = std::get_if<bool>(&value)) {
        encoder.boolean(*boolean);
    } else {
        encoder.string(std::get<std::string>(value));
    }
}

bool decodeConstant(Decoder& decoder, ConstantValue& value) {
    uint32_t tag = 0;
    if (!decoder.u32(tag) || tag > 3) return false;
    if (tag == 0) {
        int64_t decoded = 0;
        if (!decoder.i64(decoded)) return false;
        value = decoded;
    } else if (tag == 1) {
        uint64_t bits = 0;
        double decoded = 0;
        if (!decoder.u64(bits)) return false;
        static_assert(sizeof(bits) == sizeof(decoded));
        std::memcpy(&decoded, &bits, sizeof(decoded));
        value = decoded;
    } else if (tag == 2) {
        bool decoded = false;
        if (!decoder.boolean(decoded)) return false;
        value = decoded;
    } else {
        std::string decoded;
        if (!decoder.string(decoded)) return false;
        value = std::move(decoded);
    }
    return true;
}

void encodeMetadata(Encoder& encoder, const MetadataInstance& metadata) {
    encoder.string(metadata.schemaId);
    encoder.rows(metadata.values, [&](const auto& value) {
        encodeConstant(encoder, value);
    });
    encoder.enumeration(metadata.retention);
    encodeLocation(encoder, metadata.location);
}

bool decodeMetadata(Decoder& decoder, MetadataInstance& metadata) {
    if (!decoder.string(metadata.schemaId)) return false;
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    metadata.values.clear();
    for (uint32_t index = 0; index < count; ++index) {
        ConstantValue value;
        if (!decodeConstant(decoder, value)) return false;
        metadata.values.push_back(std::move(value));
    }
    return decoder.enumeration(metadata.retention, 1) &&
        decodeLocation(decoder, metadata.location);
}

std::vector<const DeclarationRecord*> sortedDeclarations(
    const Module& module, std::string& error) {
    error.clear();
    std::vector<const DeclarationRecord*> result;
    result.reserve(module.declarationTable.size());
    for (const auto& declaration : module.declarationTable)
        result.push_back(&declaration);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->symbolId.value < right->symbolId.value;
    });
    std::string previous;
    for (const auto* declaration : result) {
        if (declaration->symbolId.empty() ||
            (!previous.empty() && declaration->symbolId.value <= previous)) {
            error = "Moon Container SymbolIds are empty, duplicate, or out of order";
            return {};
        }
        previous = declaration->symbolId.value;
    }
    return result;
}

struct ContractPayload {
    luna::identity::ContractId id;
    luna::sysmeta::Facts facts;
    DeclarationRef dropGlue;
    std::string canonical;
};

struct SysmetaPayload {
    SymbolRef symbol;
    std::vector<MetadataInstance> metadata;
};


} // namespace container_detail

using namespace container_detail;

bool ContainerModelCodec::encodeManifest(
    const ContainerManifest& manifest,
    std::vector<uint8_t>& output,
    std::string& error,
    const ContainerLimits& limits) {
    if ((manifest.packageKind != ContainerPackageKind::Application &&
         manifest.packageKind != ContainerPackageKind::Library) ||
        manifest.packageId.empty() || manifest.packageVersion.empty() ||
        manifest.targetTriple.empty() || manifest.dataLayout.empty() ||
        (manifest.packageKind == ContainerPackageKind::Application &&
         !manifest.entrypoint.complete()) ||
        (manifest.packageKind == ContainerPackageKind::Library &&
         !manifest.entrypoint.empty())) {
        error = "Moon Container manifest has invalid package or entrypoint fields";
        output.clear();
        return false;
    }
    Encoder encoder(limits);
    encoder.string(manifest.packageId);
    encoder.string(manifest.packageVersion);
    encoder.enumeration(manifest.packageKind);
    encoder.string(manifest.targetTriple);
    encoder.string(manifest.dataLayout);
    encodeReference(encoder, manifest.entrypoint);
    encoder.u32(featureBits(manifest.features));
    if (!encoder.good()) {
        error = encoder.error();
        output.clear();
        return false;
    }
    output = encoder.finish();
    error.clear();
    return true;
}

bool ContainerModelCodec::decodeManifest(
    const std::vector<uint8_t>& input,
    ContainerManifest& manifest,
    std::string& error,
    const ContainerLimits& limits) {
    if (input.size() > limits.maximumContainerBytes) {
        error = "Moon Container manifest payload exceeds the configured byte limit";
        return false;
    }
    Decoder decoder(input, limits);
    ContainerManifest decoded;
    uint32_t features = 0;
    if (!decoder.string(decoded.packageId) ||
        !decoder.string(decoded.packageVersion) ||
        !decoder.enumeration(decoded.packageKind, 2) ||
        decoded.packageKind == static_cast<ContainerPackageKind>(0) ||
        !decoder.string(decoded.targetTriple) ||
        !decoder.string(decoded.dataLayout) ||
        !decodeReference(decoder, decoded.entrypoint) ||
        !decoder.u32(features) || (features & ~0x31u) != 0 ||
        !decoder.finish("manifest")) {
        error = decoder.error().empty()
            ? "Moon Container manifest has invalid flags or package kind"
            : decoder.error();
        return false;
    }
    if (decoded.packageId.empty() || decoded.packageVersion.empty() ||
        decoded.targetTriple.empty() || decoded.dataLayout.empty() ||
        (decoded.packageKind == ContainerPackageKind::Application &&
         !decoded.entrypoint.complete()) ||
        (decoded.packageKind == ContainerPackageKind::Library &&
         !decoded.entrypoint.empty())) {
        error = "Moon Container manifest has invalid package or entrypoint fields";
        return false;
    }
    decodeFeatureBits(features, decoded.features);
    manifest = std::move(decoded);
    error.clear();
    return true;
}

bool ContainerModelCodec::encodeTypes(
    const Module& module,
    std::vector<uint8_t>& output,
    std::string& error,
    const ContainerLimits& limits) {
    if (!module.typeTableSealed) {
        error = "Moon Container type table must be sealed before encoding";
        output.clear();
        return false;
    }
    std::string previousId;
    for (const auto& type : module.typeTable) {
        if (type.id.empty() ||
            (!previousId.empty() && type.id.value <= previousId)) {
            error = "Moon Container TypeIds are empty, duplicate, or out of order";
            output.clear();
            return false;
        }
        previousId = type.id.value;
    }
    Encoder encoder(limits);
    encoder.rows(module.typeTable, [&](const auto& type) {
        encodeType(encoder, type);
    });
    if (!encoder.good()) {
        error = encoder.error();
        output.clear();
        return false;
    }
    output = encoder.finish();
    error.clear();
    return true;
}

bool ContainerModelCodec::decodeTypes(
    const std::vector<uint8_t>& input,
    Module& module,
    std::string& error,
    const ContainerLimits& limits) {
    if (input.size() > limits.maximumContainerBytes) {
        error = "Moon Container type payload exceeds the configured byte limit";
        return false;
    }
    Decoder decoder(input, limits);
    uint32_t count = 0;
    if (!decoder.rowCount(count)) {
        error = decoder.error();
        return false;
    }
    std::vector<TypeRecord> decoded;
    std::string previousId;
    for (uint32_t index = 0; index < count; ++index) {
        TypeRecord type;
        if (!decodeType(decoder, type)) {
            error = decoder.error().empty()
                ? "Moon Container type record contains an invalid scalar"
                : decoder.error();
            return false;
        }
        if (type.id.empty() ||
            (!previousId.empty() && type.id.value <= previousId)) {
            error = "Moon Container TypeIds are empty, duplicate, or out of order";
            return false;
        }
        previousId = type.id.value;
        decoded.push_back(std::move(type));
    }
    if (!decoder.finish("type")) {
        error = decoder.error();
        return false;
    }
    module.typeTable = std::move(decoded);
    module.typeTableSealed = true;
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::encodeSymbols(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    auto declarations = sortedDeclarations(module, error);
    if (!error.empty()) { output.clear(); return false; }
    Encoder encoder(limits);
    encoder.rows(declarations, [&](const auto* declaration) {
        encoder.string(declaration->symbolId.value);
        encoder.string(declaration->id);
        encoder.string(declaration->familyId);
        encoder.string(declaration->sourceName);
        encoder.string(declaration->linkageName);
        encoder.enumeration(declaration->kind);
        encoder.enumeration(declaration->retention);
        encoder.string(declaration->type.value);
        encodeLocation(encoder, declaration->location);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::encodeContracts(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    auto declarations = sortedDeclarations(module, error);
    if (!error.empty()) { output.clear(); return false; }
    Encoder encoder(limits);
    encoder.rows(declarations, [&](const auto* declaration) {
        encoder.string(declaration->symbolId.value);
        encoder.string(declaration->contractId.value);
        encodeFacts(encoder, declaration->sysmeta);
        encodeReference(encoder, declaration->dropGlue);
        encoder.string(declaration->canonicalContract);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::encodeSysmeta(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    auto declarations = sortedDeclarations(module, error);
    if (!error.empty()) { output.clear(); return false; }
    std::vector<const MetadataSchema*> schemas;
    schemas.reserve(module.metadataSchemas.size());
    for (const auto& schema : module.metadataSchemas) schemas.push_back(&schema);
    std::sort(schemas.begin(), schemas.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    std::string previous;
    for (const auto* schema : schemas) {
        if (schema->id.empty() || (!previous.empty() && schema->id <= previous)) {
            error = "Moon Container metadata schema IDs are empty or duplicate";
            output.clear(); return false;
        }
        previous = schema->id;
    }
    Encoder encoder(limits);
    encoder.rows(schemas, [&](const auto* schema) {
        encoder.string(schema->id);
        encoder.string(schema->name);
        encoder.rows(schema->fields, [&](const auto& field) {
            encoder.string(field.name);
            encoder.string(field.type.value);
        });
        encodeLocation(encoder, schema->location);
    });
    encoder.rows(declarations, [&](const auto* declaration) {
        encoder.string(declaration->symbolId.value);
        encoder.rows(declaration->metadata, [&](const auto& metadata) {
            encodeMetadata(encoder, metadata);
        });
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::decodeDeclarations(
    const std::vector<uint8_t>& symbols,
    const std::vector<uint8_t>& contracts,
    const std::vector<uint8_t>& sysmeta,
    Module& module,
    std::string& error,
    const ContainerLimits& limits) {
    if (symbols.size() > limits.maximumContainerBytes ||
        contracts.size() > limits.maximumContainerBytes ||
        sysmeta.size() > limits.maximumContainerBytes) {
        error = "Moon Container declaration payload exceeds the configured byte limit";
        return false;
    }

    Decoder symbolDecoder(symbols, limits);
    uint32_t symbolCount = 0;
    if (!symbolDecoder.rowCount(symbolCount)) {
        error = symbolDecoder.error(); return false;
    }
    std::vector<DeclarationRecord> declarations;
    std::string previousSymbol;
    for (uint32_t index = 0; index < symbolCount; ++index) {
        DeclarationRecord declaration;
        if (!symbolDecoder.string(declaration.symbolId.value) ||
            !symbolDecoder.string(declaration.id) ||
            !symbolDecoder.string(declaration.familyId) ||
            !symbolDecoder.string(declaration.sourceName) ||
            !symbolDecoder.string(declaration.linkageName) ||
            !symbolDecoder.enumeration(declaration.kind, 6) ||
            !symbolDecoder.enumeration(declaration.retention, 1) ||
            !symbolDecoder.string(declaration.type.value) ||
            !decodeLocation(symbolDecoder, declaration.location)) {
            error = symbolDecoder.error().empty()
                ? "Moon Container symbol record contains an invalid scalar"
                : symbolDecoder.error();
            return false;
        }
        if (declaration.symbolId.empty() ||
            (!previousSymbol.empty() && declaration.symbolId.value <= previousSymbol)) {
            error = "Moon Container SymbolIds are empty, duplicate, or out of order";
            return false;
        }
        previousSymbol = declaration.symbolId.value;
        declarations.push_back(std::move(declaration));
    }
    if (!symbolDecoder.finish("symbol")) {
        error = symbolDecoder.error(); return false;
    }

    Decoder contractDecoder(contracts, limits);
    uint32_t contractCount = 0;
    if (!contractDecoder.rowCount(contractCount)) {
        error = contractDecoder.error(); return false;
    }
    std::unordered_map<std::string, ContractPayload> contractsBySymbol;
    previousSymbol.clear();
    for (uint32_t index = 0; index < contractCount; ++index) {
        SymbolRef symbol;
        ContractPayload payload;
        if (!contractDecoder.string(symbol.value) ||
            !contractDecoder.string(payload.id.value) ||
            !decodeFacts(contractDecoder, payload.facts) ||
            !decodeReference(contractDecoder, payload.dropGlue) ||
            !contractDecoder.string(payload.canonical)) {
            error = contractDecoder.error().empty()
                ? "Moon Container contract record contains an invalid scalar"
                : contractDecoder.error();
            return false;
        }
        if (symbol.empty() ||
            (!previousSymbol.empty() && symbol.value <= previousSymbol)) {
            error = "Moon Container contract SymbolIds are empty, duplicate, or out of order";
            return false;
        }
        previousSymbol = symbol.value;
        contractsBySymbol.emplace(symbol.value, std::move(payload));
    }
    if (!contractDecoder.finish("contract")) {
        error = contractDecoder.error(); return false;
    }

    Decoder sysmetaDecoder(sysmeta, limits);
    uint32_t schemaCount = 0;
    if (!sysmetaDecoder.rowCount(schemaCount)) {
        error = sysmetaDecoder.error(); return false;
    }
    std::vector<MetadataSchema> schemas;
    std::string previousSchema;
    for (uint32_t index = 0; index < schemaCount; ++index) {
        MetadataSchema schema;
        if (!sysmetaDecoder.string(schema.id) ||
            !sysmetaDecoder.string(schema.name)) {
            error = sysmetaDecoder.error(); return false;
        }
        uint32_t fieldCount = 0;
        if (!sysmetaDecoder.rowCount(fieldCount)) {
            error = sysmetaDecoder.error(); return false;
        }
        for (uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            MetadataField field;
            if (!sysmetaDecoder.string(field.name) ||
                !sysmetaDecoder.string(field.type.value)) {
                error = sysmetaDecoder.error(); return false;
            }
            schema.fields.push_back(std::move(field));
        }
        if (!decodeLocation(sysmetaDecoder, schema.location)) {
            error = sysmetaDecoder.error().empty()
                ? "Moon Container metadata location contains an invalid scalar"
                : sysmetaDecoder.error();
            return false;
        }
        if (schema.id.empty() ||
            (!previousSchema.empty() && schema.id <= previousSchema)) {
            error = "Moon Container metadata schema IDs are empty, duplicate, or out of order";
            return false;
        }
        previousSchema = schema.id;
        schemas.push_back(std::move(schema));
    }
    uint32_t metadataCount = 0;
    if (!sysmetaDecoder.rowCount(metadataCount)) {
        error = sysmetaDecoder.error(); return false;
    }
    std::unordered_map<std::string, SysmetaPayload> metadataBySymbol;
    previousSymbol.clear();
    for (uint32_t index = 0; index < metadataCount; ++index) {
        SysmetaPayload payload;
        if (!sysmetaDecoder.string(payload.symbol.value)) {
            error = sysmetaDecoder.error(); return false;
        }
        uint32_t instanceCount = 0;
        if (!sysmetaDecoder.rowCount(instanceCount)) {
            error = sysmetaDecoder.error(); return false;
        }
        for (uint32_t instanceIndex = 0; instanceIndex < instanceCount;
             ++instanceIndex) {
            MetadataInstance metadata;
            if (!decodeMetadata(sysmetaDecoder, metadata)) {
                error = sysmetaDecoder.error().empty()
                    ? "Moon Container metadata value contains an invalid scalar"
                    : sysmetaDecoder.error();
                return false;
            }
            payload.metadata.push_back(std::move(metadata));
        }
        if (payload.symbol.empty() ||
            (!previousSymbol.empty() && payload.symbol.value <= previousSymbol)) {
            error = "Moon Container sysmeta SymbolIds are empty, duplicate, or out of order";
            return false;
        }
        previousSymbol = payload.symbol.value;
        metadataBySymbol.emplace(payload.symbol.value, std::move(payload));
    }
    if (!sysmetaDecoder.finish("sysmeta")) {
        error = sysmetaDecoder.error(); return false;
    }

    if (declarations.size() != contractsBySymbol.size() ||
        declarations.size() != metadataBySymbol.size()) {
        error = "Moon Container symbol, contract, and sysmeta key sets differ";
        return false;
    }
    for (auto& declaration : declarations) {
        auto contract = contractsBySymbol.find(declaration.symbolId.value);
        auto metadata = metadataBySymbol.find(declaration.symbolId.value);
        if (contract == contractsBySymbol.end() ||
            metadata == metadataBySymbol.end()) {
            error = "Moon Container declaration section is missing a SymbolId key";
            return false;
        }
        declaration.contractId = std::move(contract->second.id);
        declaration.sysmeta = std::move(contract->second.facts);
        declaration.dropGlue = std::move(contract->second.dropGlue);
        declaration.canonicalContract = std::move(contract->second.canonical);
        declaration.metadata = std::move(metadata->second.metadata);
        if (luna::identity::symbolIdFromCanonical(declaration.id) !=
                declaration.symbolId ||
            declaration.sysmeta.identity.symbol != declaration.symbolId ||
            declaration.sysmeta.identity.contract != declaration.contractId ||
            canonicalContract(declaration) != declaration.canonicalContract ||
            luna::identity::contractIdFromCanonical(
                declaration.canonicalContract) != declaration.contractId) {
            error = "Moon Container declaration identity or contract payload mismatch";
            return false;
        }
    }

    module.declarationTable = std::move(declarations);
    module.metadataSchemas = std::move(schemas);
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::encodeImports(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    std::string previousKey;
    for (const auto& import : module.imports) {
        const std::string key =
            std::to_string(static_cast<unsigned>(import.kind)) + "\n" +
            import.ownerPackageId + "\n" + import.localName + "\n" +
            import.packageId + "\n" + import.alias;
        if (!previousKey.empty() && key <= previousKey) {
            error = "Moon Container imports are duplicate or out of order";
            output.clear(); return false;
        }
        previousKey = key;
    }
    Encoder encoder(limits);
    encoder.rows(module.imports, [&](const auto& import) {
        encoder.enumeration(import.kind);
        encoder.string(import.ownerPackageId);
        encoder.string(import.localName);
        encoder.string(import.packageId);
        encoder.string(import.alias);
        encoder.string(import.capabilityId);
        encoder.string(import.linkSymbol);
        encoder.string(import.abi);
        encodeReference(encoder, import.declaration);
        encoder.string(import.type.value);
        encodeLocation(encoder, import.location);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::encodeExports(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    std::string previousKey;
    for (const auto& exported : module.exports) {
        const std::string key = exported.name + "\n" +
            exported.declaration.symbol.value;
        if (!previousKey.empty() && key <= previousKey) {
            error = "Moon Container exports are duplicate or out of order";
            output.clear(); return false;
        }
        previousKey = key;
    }
    Encoder encoder(limits);
    encoder.rows(module.exports, [&](const auto& exported) {
        encoder.string(exported.name);
        encodeReference(encoder, exported.declaration);
        encoder.string(exported.type.value);
        encoder.enumeration(exported.kind);
        encoder.string(exported.abi);
        encodeLocation(encoder, exported.location);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::decodeInterfaces(
    const std::vector<uint8_t>& imports,
    const std::vector<uint8_t>& exports,
    Module& module, std::string& error,
    const ContainerLimits& limits) {
    if (imports.size() > limits.maximumContainerBytes ||
        exports.size() > limits.maximumContainerBytes) {
        error = "Moon Container interface payload exceeds the configured byte limit";
        return false;
    }
    Decoder importDecoder(imports, limits);
    uint32_t importCount = 0;
    if (!importDecoder.rowCount(importCount)) {
        error = importDecoder.error(); return false;
    }
    std::vector<ImportRecord> decodedImports;
    std::string previousKey;
    for (uint32_t index = 0; index < importCount; ++index) {
        ImportRecord import;
        if (!importDecoder.enumeration(import.kind, 1) ||
            !importDecoder.string(import.ownerPackageId) ||
            !importDecoder.string(import.localName) ||
            !importDecoder.string(import.packageId) ||
            !importDecoder.string(import.alias) ||
            !importDecoder.string(import.capabilityId) ||
            !importDecoder.string(import.linkSymbol) ||
            !importDecoder.string(import.abi) ||
            !decodeReference(importDecoder, import.declaration) ||
            !importDecoder.string(import.type.value) ||
            !decodeLocation(importDecoder, import.location)) {
            error = importDecoder.error().empty()
                ? "Moon Container import contains an invalid scalar"
                : importDecoder.error();
            return false;
        }
        const std::string key =
            std::to_string(static_cast<unsigned>(import.kind)) + "\n" +
            import.ownerPackageId + "\n" + import.localName + "\n" +
            import.packageId + "\n" + import.alias;
        if (!previousKey.empty() && key <= previousKey) {
            error = "Moon Container imports are duplicate or out of order";
            return false;
        }
        previousKey = key;
        decodedImports.push_back(std::move(import));
    }
    if (!importDecoder.finish("imports")) {
        error = importDecoder.error(); return false;
    }

    Decoder exportDecoder(exports, limits);
    uint32_t exportCount = 0;
    if (!exportDecoder.rowCount(exportCount)) {
        error = exportDecoder.error(); return false;
    }
    std::vector<ExportRecord> decodedExports;
    previousKey.clear();
    for (uint32_t index = 0; index < exportCount; ++index) {
        ExportRecord exported;
        if (!exportDecoder.string(exported.name) ||
            !decodeReference(exportDecoder, exported.declaration) ||
            !exportDecoder.string(exported.type.value) ||
            !exportDecoder.enumeration(exported.kind, 6) ||
            !exportDecoder.string(exported.abi) ||
            !decodeLocation(exportDecoder, exported.location)) {
            error = exportDecoder.error().empty()
                ? "Moon Container export contains an invalid scalar"
                : exportDecoder.error();
            return false;
        }
        const std::string key = exported.name + "\n" +
            exported.declaration.symbol.value;
        if (!previousKey.empty() && key <= previousKey) {
            error = "Moon Container exports are duplicate or out of order";
            return false;
        }
        previousKey = key;
        decodedExports.push_back(std::move(exported));
    }
    if (!exportDecoder.finish("exports")) {
        error = exportDecoder.error(); return false;
    }

    std::vector<Module::PackageUse> packageUses;
    for (const auto& import : decodedImports)
        if (import.kind == ImportKind::Package)
            packageUses.push_back({import.ownerPackageId,
                                   import.packageId, import.alias});
    module.imports = std::move(decodedImports);
    module.exports = std::move(decodedExports);
    module.packageUses = std::move(packageUses);
    error.clear();
    return true;
}


} // namespace moon
