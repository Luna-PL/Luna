#include "ContainerModel.h"
#include "Verifier.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <utility>

namespace moon {
namespace {

bool isValidUtf8(const uint8_t* bytes, size_t size) {
    size_t index = 0;
    while (index < size) {
        const uint8_t lead = bytes[index++];
        if (lead <= 0x7f) continue;
        uint32_t codePoint = 0;
        size_t continuationCount = 0;
        uint32_t minimum = 0;
        if ((lead & 0xe0u) == 0xc0u) {
            codePoint = lead & 0x1fu;
            continuationCount = 1;
            minimum = 0x80;
        } else if ((lead & 0xf0u) == 0xe0u) {
            codePoint = lead & 0x0fu;
            continuationCount = 2;
            minimum = 0x800;
        } else if ((lead & 0xf8u) == 0xf0u) {
            codePoint = lead & 0x07u;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuationCount > size - index) return false;
        for (size_t offset = 0; offset < continuationCount; ++offset) {
            const uint8_t continuation = bytes[index++];
            if ((continuation & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6) | (continuation & 0x3fu);
        }
        if (codePoint < minimum || codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
    }
    return true;
}

class Encoder {
public:
    explicit Encoder(const ContainerLimits& limits) : mLimits(limits) {}

    bool good() const { return mError.empty(); }
    const std::string& error() const { return mError; }
    std::vector<uint8_t> finish() { return std::move(mBytes); }
    bool reject(std::string message) {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    void u32(uint32_t value) {
        if (!reserveBytes(4)) return;
        for (unsigned index = 0; index < 4; ++index)
            mBytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
    }

    void u64(uint64_t value) {
        if (!reserveBytes(8)) return;
        for (unsigned index = 0; index < 8; ++index)
            mBytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
    }

    void i64(int64_t value) { u64(static_cast<uint64_t>(value)); }

    void boolean(bool value) { u32(value ? 1u : 0u); }

    template <typename Enum>
    void enumeration(Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<uint32_t>(value));
    }

    void string(const std::string& value) {
        if (!good()) return;
        if (value.size() > mLimits.maximumStringBytes ||
            value.size() > std::numeric_limits<uint32_t>::max()) {
            mError = "Moon Container string exceeds the configured byte limit";
            return;
        }
        if (!isValidUtf8(
                reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
            mError = "Moon Container string is not valid UTF-8";
            return;
        }
        if (!reserveBytes(4 + value.size())) return;
        // The whole field was checked above; write directly to avoid counting
        // its length prefix twice against the payload limit.
        for (unsigned index = 0; index < 4; ++index)
            mBytes.push_back(static_cast<uint8_t>(
                static_cast<uint32_t>(value.size()) >> (index * 8)));
        mBytes.insert(mBytes.end(), value.begin(), value.end());
    }

    template <typename Range, typename Function>
    void rows(const Range& values, Function encode) {
        if (!good()) return;
        if (values.size() > mLimits.maximumTableRows ||
            values.size() > std::numeric_limits<uint32_t>::max()) {
            mError = "Moon Container table exceeds the configured row limit";
            return;
        }
        u32(static_cast<uint32_t>(values.size()));
        for (const auto& value : values) encode(value);
    }

private:
    bool reserveBytes(size_t size) {
        if (!good()) return false;
        if (size > mLimits.maximumContainerBytes ||
            mBytes.size() > mLimits.maximumContainerBytes - size) {
            mError = "Moon Container payload exceeds the configured byte limit";
            return false;
        }
        return true;
    }

    const ContainerLimits& mLimits;
    std::vector<uint8_t> mBytes;
    std::string mError;
};

class Decoder {
public:
    Decoder(const std::vector<uint8_t>& bytes, const ContainerLimits& limits)
        : mBytes(bytes), mLimits(limits) {}

    bool good() const { return mError.empty(); }
    bool atEnd() const { return mOffset == mBytes.size(); }
    const std::string& error() const { return mError; }
    bool reject(std::string message) { return fail(std::move(message)); }

    bool finish(const char* section) {
        if (!good()) return false;
        if (!atEnd())
            return fail(std::string(section) + " section has trailing bytes");
        return true;
    }

    bool u32(uint32_t& value) {
        if (!require(4)) return false;
        value = 0;
        for (unsigned index = 0; index < 4; ++index)
            value |= static_cast<uint32_t>(mBytes[mOffset++]) << (index * 8);
        return true;
    }

    bool u64(uint64_t& value) {
        if (!require(8)) return false;
        value = 0;
        for (unsigned index = 0; index < 8; ++index)
            value |= static_cast<uint64_t>(mBytes[mOffset++]) << (index * 8);
        return true;
    }

    bool i64(int64_t& value) {
        uint64_t bits = 0;
        if (!u64(bits)) return false;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool boolean(bool& value) {
        uint32_t encoded = 0;
        if (!u32(encoded)) return false;
        if (encoded > 1) return fail("Moon Container boolean is not 0 or 1");
        value = encoded != 0;
        return true;
    }

    template <typename Enum>
    bool enumeration(Enum& value, uint32_t maximum) {
        static_assert(std::is_enum_v<Enum>);
        uint32_t encoded = 0;
        if (!u32(encoded)) return false;
        if (encoded > maximum)
            return fail("Moon Container enum value is out of range");
        value = static_cast<Enum>(encoded);
        return true;
    }

    bool string(std::string& value) {
        uint32_t size = 0;
        if (!u32(size)) return false;
        if (size > mLimits.maximumStringBytes)
            return fail("Moon Container string exceeds the configured byte limit");
        if (!require(size)) return false;
        if (!isValidUtf8(mBytes.data() + mOffset, size))
            return fail("Moon Container string is not valid UTF-8");
        value.assign(reinterpret_cast<const char*>(mBytes.data() + mOffset), size);
        mOffset += size;
        return true;
    }

    bool rowCount(uint32_t& count) {
        if (!u32(count)) return false;
        if (count > mLimits.maximumTableRows)
            return fail("Moon Container table exceeds the configured row limit");
        // Every current row has at least one u32 field. This prevents a tiny
        // hostile payload from driving a huge reserve/iteration before the
        // first row can be proven present.
        if (count > (mBytes.size() - mOffset) / 4)
            return fail("Moon Container section payload is truncated");
        return true;
    }

private:
    bool require(size_t size) {
        if (size > mBytes.size() - mOffset)
            return fail("Moon Container section payload is truncated");
        return true;
    }

    bool fail(std::string message) {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    const std::vector<uint8_t>& mBytes;
    const ContainerLimits& mLimits;
    size_t mOffset = 0;
    std::string mError;
};

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

template <typename Reference>
void encodeTypeRefs(Encoder& encoder, const std::vector<Reference>& references) {
    encoder.rows(references, [&](const auto& reference) {
        encoder.string(reference.value);
    });
}

template <typename Reference>
bool decodeTypeRefs(Decoder& decoder, std::vector<Reference>& references) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    references.clear();
    for (uint32_t index = 0; index < count; ++index) {
        Reference reference;
        if (!decoder.string(reference.value)) return false;
        references.push_back(std::move(reference));
    }
    return true;
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

} // namespace

std::optional<CodeOperationOpcode> codeOperationOpcode(const Stmt& operation) {
    if (dynamic_cast<const LetStmt*>(&operation))
        return CodeOperationOpcode::Let;
    if (dynamic_cast<const AllocateStmt*>(&operation))
        return CodeOperationOpcode::Allocate;
    if (dynamic_cast<const ExprStmt*>(&operation))
        return CodeOperationOpcode::Expression;
    if (dynamic_cast<const FreeStmt*>(&operation))
        return CodeOperationOpcode::Free;
    if (dynamic_cast<const AwaitStmt*>(&operation))
        return CodeOperationOpcode::Await;
    return std::nullopt;
}

std::optional<CodeExpressionOpcode> codeExpressionOpcode(
    const Expr& expression) {
    if (dynamic_cast<const IntLiteralExpr*>(&expression)) return CodeExpressionOpcode::Integer;
    if (dynamic_cast<const FloatLiteralExpr*>(&expression)) return CodeExpressionOpcode::Floating;
    if (dynamic_cast<const StringLiteralExpr*>(&expression)) return CodeExpressionOpcode::String;
    if (dynamic_cast<const BoolLiteralExpr*>(&expression)) return CodeExpressionOpcode::Boolean;
    if (dynamic_cast<const UnitExpr*>(&expression)) return CodeExpressionOpcode::Unit;
    if (dynamic_cast<const IdentifierExpr*>(&expression)) return CodeExpressionOpcode::Identifier;
    if (dynamic_cast<const BinaryExpr*>(&expression)) return CodeExpressionOpcode::Binary;
    if (dynamic_cast<const UnaryExpr*>(&expression)) return CodeExpressionOpcode::Unary;
    if (dynamic_cast<const CallExpr*>(&expression)) return CodeExpressionOpcode::Call;
    if (dynamic_cast<const LaunchExpr*>(&expression)) return CodeExpressionOpcode::Launch;
    if (dynamic_cast<const VariantConstructExpr*>(&expression)) return CodeExpressionOpcode::VariantConstruct;
    if (dynamic_cast<const ResultConstructExpr*>(&expression)) return CodeExpressionOpcode::ResultConstruct;
    if (dynamic_cast<const FieldAccessExpr*>(&expression)) return CodeExpressionOpcode::FieldAccess;
    if (dynamic_cast<const IndexExpr*>(&expression)) return CodeExpressionOpcode::Index;
    if (dynamic_cast<const SliceLengthExpr*>(&expression)) return CodeExpressionOpcode::SliceLength;
    if (dynamic_cast<const ArrayLiteralExpr*>(&expression)) return CodeExpressionOpcode::ArrayLiteral;
    if (dynamic_cast<const RecordLiteralExpr*>(&expression)) return CodeExpressionOpcode::RecordLiteral;
    if (dynamic_cast<const HeapAllocExpr*>(&expression)) return CodeExpressionOpcode::HeapAllocate;
    if (dynamic_cast<const InitAllocationExpr*>(&expression)) return CodeExpressionOpcode::InitializeAllocation;
    if (dynamic_cast<const MoveExpr*>(&expression)) return CodeExpressionOpcode::Move;
    if (dynamic_cast<const BorrowExpr*>(&expression)) return CodeExpressionOpcode::Borrow;
    if (dynamic_cast<const DerefExpr*>(&expression)) return CodeExpressionOpcode::Dereference;
    if (dynamic_cast<const AddrOfExpr*>(&expression)) return CodeExpressionOpcode::AddressOf;
    if (dynamic_cast<const LambdaExpr*>(&expression)) return CodeExpressionOpcode::Lambda;
    if (dynamic_cast<const MakeClosureExpr*>(&expression)) return CodeExpressionOpcode::MakeClosure;
    if (dynamic_cast<const EnvLoadExpr*>(&expression)) return CodeExpressionOpcode::EnvironmentLoad;
    if (dynamic_cast<const AssignExpr*>(&expression)) return CodeExpressionOpcode::Assign;
    return std::nullopt;
}

namespace {

template <typename Ref>
void encodeTableRef(Encoder& encoder, Ref reference) {
    encoder.u32(reference.value);
}

template <typename Ref>
bool decodeTableRef(Decoder& decoder, Ref& reference) {
    return decoder.u32(reference.value);
}

template <typename Ref>
void encodeTableRefs(Encoder& encoder, const std::vector<Ref>& references) {
    encoder.rows(references, [&](const auto& reference) {
        encodeTableRef(encoder, reference);
    });
}

template <typename Ref>
bool decodeTableRefs(Decoder& decoder, std::vector<Ref>& references) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    references.clear();
    for (uint32_t index = 0; index < count; ++index) {
        Ref reference;
        if (!decodeTableRef(decoder, reference)) return false;
        references.push_back(reference);
    }
    return true;
}

void encodeParam(Encoder& encoder, const Param& parameter) {
    encoder.string(parameter.name);
    encoder.boolean(parameter.isLinear);
    encoder.enumeration(parameter.usage);
    encoder.enumeration(parameter.relation);
    encoder.string(parameter.type.value);
}

bool decodeParam(Decoder& decoder, Param& parameter) {
    return decoder.string(parameter.name) &&
        decoder.boolean(parameter.isLinear) &&
        decoder.enumeration(parameter.usage, 2) &&
        decoder.enumeration(parameter.relation, 2) &&
        decoder.string(parameter.type.value);
}

bool encodeGraph(Encoder&, const ControlFlowGraph&, uint32_t,
                 const ContainerLimits&);
bool decodeGraph(Decoder&, std::unique_ptr<ControlFlowGraph>&, uint32_t,
                 const ContainerLimits&);

bool encodeExpr(Encoder& encoder, const Expr* expression, uint32_t depth,
                const ContainerLimits& limits);
bool decodeExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                uint32_t depth, const ContainerLimits& limits);

bool encodeOptionalExpr(Encoder& encoder, const Expr* expression,
                        uint32_t depth, const ContainerLimits& limits) {
    encoder.boolean(expression != nullptr);
    return !expression || encodeExpr(encoder, expression, depth, limits);
}

bool decodeOptionalExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                        uint32_t depth, const ContainerLimits& limits) {
    bool present = false;
    if (!decoder.boolean(present)) return false;
    if (!present) { expression.reset(); return true; }
    return decodeExpr(decoder, expression, depth, limits);
}

bool encodeExprVector(
    Encoder& encoder, const std::vector<std::unique_ptr<Expr>>& values,
    uint32_t depth, const ContainerLimits& limits) {
    encoder.rows(values, [&](const auto& value) {
        if (encoder.good()) encodeOptionalExpr(
            encoder, value.get(), depth, limits);
    });
    return encoder.good();
}

bool decodeExprVector(
    Decoder& decoder, std::vector<std::unique_ptr<Expr>>& values,
    uint32_t depth, const ContainerLimits& limits) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    values.clear();
    for (uint32_t index = 0; index < count; ++index) {
        std::unique_ptr<Expr> value;
        if (!decodeOptionalExpr(decoder, value, depth, limits)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

bool encodeExpr(Encoder& encoder, const Expr* expression, uint32_t depth,
                const ContainerLimits& limits) {
    if (!expression) return encoder.reject(
        "Moon Container code contains a null required expression");
    if (depth >= limits.maximumNestingDepth)
        return encoder.reject("Moon Container code exceeds the nesting limit");
    const auto opcode = codeExpressionOpcode(*expression);
    if (!opcode)
        return encoder.reject(
            "Moon Container code contains a structured-only expression");
    encoder.enumeration(*opcode);
    encoder.string(expression->type.value);
    encodeLocation(encoder, expression->location);
    const uint32_t nested = depth + 1;

    if (const auto* value = dynamic_cast<const IntLiteralExpr*>(expression)) {
        encoder.i64(value->value);
    } else if (const auto* value =
                   dynamic_cast<const FloatLiteralExpr*>(expression)) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value->value, sizeof(bits));
        encoder.u64(bits);
    } else if (const auto* value =
                   dynamic_cast<const StringLiteralExpr*>(expression)) {
        encoder.string(value->value);
    } else if (const auto* value =
                   dynamic_cast<const BoolLiteralExpr*>(expression)) {
        encoder.boolean(value->value);
    } else if (dynamic_cast<const UnitExpr*>(expression)) {
    } else if (const auto* value =
                   dynamic_cast<const IdentifierExpr*>(expression)) {
        encoder.string(value->name);
        encodeTableRef(encoder, value->local);
        encodeReference(encoder, value->declaration);
    } else if (const auto* value =
                   dynamic_cast<const BinaryExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->lhs.get(), nested, limits)) return false;
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->rhs.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const UnaryExpr*>(expression)) {
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const CallExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->callee.get(), nested, limits) ||
            !encodeExprVector(encoder, value->args, nested, limits)) return false;
        encodeTypeRefs(encoder, value->typeArgs);
        encodeReference(encoder, value->calleeRef);
        encoder.boolean(value->returnsLinear);
        encoder.enumeration(value->returnUsage);
        encoder.string(value->intrinsicType.value);
        encoder.string(value->iteratorInputType.value);
        encoder.string(value->iteratorOutputType.value);
        encoder.enumeration(value->iteratorOp);
        encoder.string(value->iteratorRecipeStateName);
        encoder.string(value->iteratorRecipeSourceType.value);
        encoder.string(value->iteratorCollectTargetType.value);
        encoder.string(value->iteratorCollectBuilderType.value);
        encodeReference(encoder, value->iteratorCollectBegin);
        encodeReference(encoder, value->iteratorCollectPush);
        encodeReference(encoder, value->iteratorCollectFinish);
        encoder.boolean(value->compileTimeValue.has_value());
        if (value->compileTimeValue) encodeConstant(
            encoder, *value->compileTimeValue);
    } else if (const auto* value = dynamic_cast<const LaunchExpr*>(expression)) {
        encoder.string(value->kernelName);
        encodeReference(encoder, value->kernelRef);
        if (!encodeOptionalExpr(encoder, value->threads.get(), nested, limits) ||
            !encodeExprVector(encoder, value->args, nested, limits)) return false;
        encoder.rows(value->inFlightResources, [&](const auto& resource) {
            encoder.string(resource.first);
            encoder.boolean(resource.second);
        });
    } else if (const auto* value =
                   dynamic_cast<const VariantConstructExpr*>(expression)) {
        encoder.string(value->typeName);
        encoder.string(value->variantName);
        if (!encodeExprVector(encoder, value->args, nested, limits)) return false;
        encoder.string(value->constructedType.value);
    } else if (const auto* value =
                   dynamic_cast<const ResultConstructExpr*>(expression)) {
        encoder.boolean(value->isOk);
        if (!encodeOptionalExpr(encoder, value->payload.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const FieldAccessExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->object.get(), nested, limits)) return false;
        encoder.string(value->field);
    } else if (const auto* value = dynamic_cast<const IndexExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->object.get(), nested, limits) ||
            !encodeOptionalExpr(encoder, value->index.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const SliceLengthExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->slice.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        if (!encodeExprVector(encoder, value->elements, nested, limits)) return false;
        encoder.string(value->elementType.value);
    } else if (const auto* value =
                   dynamic_cast<const RecordLiteralExpr*>(expression)) {
        encoder.rows(value->fields, [&](const auto& field) {
            encoder.string(field.name);
            if (encoder.good()) encodeOptionalExpr(
                encoder, field.value.get(), nested, limits);
        });
    } else if (const auto* value =
                   dynamic_cast<const HeapAllocExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->initializer.get(), nested, limits)) return false;
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
    } else if (const auto* value =
                   dynamic_cast<const InitAllocationExpr*>(expression)) {
        encodeTableRef(encoder, value->allocation);
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
        encoder.rows(value->elements, [&](const auto& element) {
            encoder.u32(element.index);
            if (encoder.good()) encodeOptionalExpr(
                encoder, element.value.get(), nested, limits);
        });
    } else if (const auto* value = dynamic_cast<const MoveExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
        encodeTableRef(encoder, value->nextUnread);
    } else if (const auto* value = dynamic_cast<const BorrowExpr*>(expression)) {
        encoder.boolean(value->isMutable);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const DerefExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const AddrOfExpr*>(expression)) {
        encoder.boolean(value->isMutable);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const LambdaExpr*>(expression)) {
        if (value->body)
            return encoder.reject("Moon Container lambda retains a structured body");
        encoder.rows(value->params, [&](const auto& parameter) {
            encodeParam(encoder, parameter);
        });
        encoder.string(value->returnType.value);
        encoder.boolean(value->controlFlow != nullptr);
        if (value->controlFlow &&
            !encodeGraph(encoder, *value->controlFlow, nested, limits)) return false;
        encoder.string(value->closureType.value);
        encoder.rows(value->captures, [&](const auto& capture) {
            encoder.string(capture);
        });
        encoder.string(value->identitySuffix);
        encoder.string(value->envParamName);
    } else if (const auto* value =
                   dynamic_cast<const MakeClosureExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->lambda.get(), nested, limits) ||
            !encodeExprVector(encoder, value->capturedValues, nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const EnvLoadExpr*>(expression)) {
        encodeTableRef(encoder, value->envLocal);
        encoder.u64(value->fieldIndex);
    } else if (const auto* value = dynamic_cast<const AssignExpr*>(expression)) {
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->lhs.get(), nested, limits) ||
            !encodeOptionalExpr(encoder, value->rhs.get(), nested, limits)) return false;
    }
    return encoder.good();
}

bool decodeExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return decoder.reject("Moon Container code exceeds the nesting limit");
    CodeExpressionOpcode opcode;
    if (!decoder.enumeration(
            opcode, static_cast<uint32_t>(CodeExpressionOpcode::Assign)) ||
        static_cast<uint32_t>(opcode) == 0)
        return false;

    std::unique_ptr<Expr> decoded;
    switch (opcode) {
        case CodeExpressionOpcode::Integer: decoded = std::make_unique<IntLiteralExpr>(); break;
        case CodeExpressionOpcode::Floating: decoded = std::make_unique<FloatLiteralExpr>(); break;
        case CodeExpressionOpcode::String: decoded = std::make_unique<StringLiteralExpr>(); break;
        case CodeExpressionOpcode::Boolean: decoded = std::make_unique<BoolLiteralExpr>(); break;
        case CodeExpressionOpcode::Unit: decoded = std::make_unique<UnitExpr>(); break;
        case CodeExpressionOpcode::Identifier: decoded = std::make_unique<IdentifierExpr>(); break;
        case CodeExpressionOpcode::Binary: decoded = std::make_unique<BinaryExpr>(); break;
        case CodeExpressionOpcode::Unary: decoded = std::make_unique<UnaryExpr>(); break;
        case CodeExpressionOpcode::Call: decoded = std::make_unique<CallExpr>(); break;
        case CodeExpressionOpcode::ReservedDynamicSelect: return false;
        case CodeExpressionOpcode::Launch: decoded = std::make_unique<LaunchExpr>(); break;
        case CodeExpressionOpcode::VariantConstruct: decoded = std::make_unique<VariantConstructExpr>(); break;
        case CodeExpressionOpcode::ResultConstruct: decoded = std::make_unique<ResultConstructExpr>(); break;
        case CodeExpressionOpcode::FieldAccess: decoded = std::make_unique<FieldAccessExpr>(); break;
        case CodeExpressionOpcode::Index: decoded = std::make_unique<IndexExpr>(); break;
        case CodeExpressionOpcode::SliceLength: decoded = std::make_unique<SliceLengthExpr>(); break;
        case CodeExpressionOpcode::ArrayLiteral: decoded = std::make_unique<ArrayLiteralExpr>(); break;
        case CodeExpressionOpcode::RecordLiteral: decoded = std::make_unique<RecordLiteralExpr>(); break;
        case CodeExpressionOpcode::HeapAllocate: decoded = std::make_unique<HeapAllocExpr>(); break;
        case CodeExpressionOpcode::InitializeAllocation: decoded = std::make_unique<InitAllocationExpr>(); break;
        case CodeExpressionOpcode::Move: decoded = std::make_unique<MoveExpr>(); break;
        case CodeExpressionOpcode::Borrow: decoded = std::make_unique<BorrowExpr>(); break;
        case CodeExpressionOpcode::Dereference: decoded = std::make_unique<DerefExpr>(); break;
        case CodeExpressionOpcode::AddressOf: decoded = std::make_unique<AddrOfExpr>(); break;
        case CodeExpressionOpcode::Lambda: decoded = std::make_unique<LambdaExpr>(); break;
        case CodeExpressionOpcode::MakeClosure: decoded = std::make_unique<MakeClosureExpr>(); break;
        case CodeExpressionOpcode::EnvironmentLoad: decoded = std::make_unique<EnvLoadExpr>(); break;
        case CodeExpressionOpcode::Assign: decoded = std::make_unique<AssignExpr>(); break;
    }
    if (!decoded || !decoder.string(decoded->type.value) ||
        !decodeLocation(decoder, decoded->location))
        return false;
    const uint32_t nested = depth + 1;

    if (auto* value = dynamic_cast<IntLiteralExpr*>(decoded.get())) {
        if (!decoder.i64(value->value)) return false;
    } else if (auto* value = dynamic_cast<FloatLiteralExpr*>(decoded.get())) {
        uint64_t bits = 0;
        if (!decoder.u64(bits)) return false;
        std::memcpy(&value->value, &bits, sizeof(bits));
    } else if (auto* value = dynamic_cast<StringLiteralExpr*>(decoded.get())) {
        if (!decoder.string(value->value)) return false;
    } else if (auto* value = dynamic_cast<BoolLiteralExpr*>(decoded.get())) {
        if (!decoder.boolean(value->value)) return false;
    } else if (dynamic_cast<UnitExpr*>(decoded.get())) {
    } else if (auto* value = dynamic_cast<IdentifierExpr*>(decoded.get())) {
        if (!decoder.string(value->name) ||
            !decodeTableRef(decoder, value->local) ||
            !decodeReference(decoder, value->declaration)) return false;
    } else if (auto* value = dynamic_cast<BinaryExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->lhs, nested, limits) ||
            !decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->rhs, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<UnaryExpr*>(decoded.get())) {
        if (!decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<CallExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->callee, nested, limits) ||
            !decodeExprVector(decoder, value->args, nested, limits) ||
            !decodeTypeRefs(decoder, value->typeArgs) ||
            !decodeReference(decoder, value->calleeRef) ||
            !decoder.boolean(value->returnsLinear) ||
            !decoder.enumeration(value->returnUsage, 2) ||
            !decoder.string(value->intrinsicType.value) ||
            !decoder.string(value->iteratorInputType.value) ||
            !decoder.string(value->iteratorOutputType.value) ||
            !decoder.enumeration(
                value->iteratorOp, static_cast<uint32_t>(IteratorOp::Collect)) ||
            !decoder.string(value->iteratorRecipeStateName) ||
            !decoder.string(value->iteratorRecipeSourceType.value) ||
            !decoder.string(value->iteratorCollectTargetType.value) ||
            !decoder.string(value->iteratorCollectBuilderType.value) ||
            !decodeReference(decoder, value->iteratorCollectBegin) ||
            !decodeReference(decoder, value->iteratorCollectPush) ||
            !decodeReference(decoder, value->iteratorCollectFinish)) return false;
        bool hasConstant = false;
        if (!decoder.boolean(hasConstant)) return false;
        if (hasConstant) {
            ConstantValue constant;
            if (!decodeConstant(decoder, constant)) return false;
            value->compileTimeValue = std::move(constant);
        }
    } else if (auto* value = dynamic_cast<LaunchExpr*>(decoded.get())) {
        if (!decoder.string(value->kernelName) ||
            !decodeReference(decoder, value->kernelRef) ||
            !decodeOptionalExpr(decoder, value->threads, nested, limits) ||
            !decodeExprVector(decoder, value->args, nested, limits)) return false;
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            std::pair<std::string, bool> resource;
            if (!decoder.string(resource.first) ||
                !decoder.boolean(resource.second)) return false;
            value->inFlightResources.push_back(std::move(resource));
        }
    } else if (auto* value = dynamic_cast<VariantConstructExpr*>(decoded.get())) {
        if (!decoder.string(value->typeName) ||
            !decoder.string(value->variantName) ||
            !decodeExprVector(decoder, value->args, nested, limits) ||
            !decoder.string(value->constructedType.value)) return false;
    } else if (auto* value = dynamic_cast<ResultConstructExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isOk) ||
            !decodeOptionalExpr(decoder, value->payload, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<FieldAccessExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->object, nested, limits) ||
            !decoder.string(value->field)) return false;
    } else if (auto* value = dynamic_cast<IndexExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->object, nested, limits) ||
            !decodeOptionalExpr(decoder, value->index, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<SliceLengthExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->slice, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<ArrayLiteralExpr*>(decoded.get())) {
        if (!decodeExprVector(decoder, value->elements, nested, limits) ||
            !decoder.string(value->elementType.value)) return false;
    } else if (auto* value = dynamic_cast<RecordLiteralExpr*>(decoded.get())) {
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            RecordLiteralExpr::Field field;
            if (!decoder.string(field.name) ||
                !decodeOptionalExpr(decoder, field.value, nested, limits)) return false;
            value->fields.push_back(std::move(field));
        }
    } else if (auto* value = dynamic_cast<HeapAllocExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->initializer, nested, limits) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
    } else if (auto* value = dynamic_cast<InitAllocationExpr*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->allocation) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            InitAllocationExpr::Element element;
            if (!decoder.u32(element.index) ||
                !decodeOptionalExpr(
                    decoder, element.value, nested, limits)) return false;
            value->elements.push_back(std::move(element));
        }
    } else if (auto* value = dynamic_cast<MoveExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, nested, limits) ||
            !decodeTableRef(decoder, value->nextUnread)) return false;
    } else if (auto* value = dynamic_cast<BorrowExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isMutable) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<DerefExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<AddrOfExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isMutable) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<LambdaExpr*>(decoded.get())) {
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            Param parameter;
            if (!decodeParam(decoder, parameter)) return false;
            value->params.push_back(std::move(parameter));
        }
        if (!decoder.string(value->returnType.value)) return false;
        bool hasGraph = false;
        if (!decoder.boolean(hasGraph)) return false;
        if (hasGraph &&
            !decodeGraph(decoder, value->controlFlow, nested, limits)) return false;
        if (!decoder.string(value->closureType.value) ||
            !decodeStringRows(decoder, value->captures) ||
            !decoder.string(value->identitySuffix) ||
            !decoder.string(value->envParamName)) return false;
    } else if (auto* value = dynamic_cast<MakeClosureExpr*>(decoded.get())) {
        std::unique_ptr<Expr> lambda;
        if (!decodeOptionalExpr(decoder, lambda, nested, limits)) return false;
        if (lambda) {
            auto* typed = dynamic_cast<LambdaExpr*>(lambda.get());
            if (!typed)
                return decoder.reject(
                    "Moon Container MakeClosure payload is not a Lambda");
            lambda.release();
            value->lambda.reset(typed);
        }
        if (!decodeExprVector(
                decoder, value->capturedValues, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<EnvLoadExpr*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->envLocal) ||
            !decoder.u64(value->fieldIndex)) return false;
    } else if (auto* value = dynamic_cast<AssignExpr*>(decoded.get())) {
        if (!decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->lhs, nested, limits) ||
            !decodeOptionalExpr(decoder, value->rhs, nested, limits)) return false;
    }
    expression = std::move(decoded);
    return true;
}

void encodePlace(Encoder& encoder, const PlaceRef& place) {
    encodeTableRef(encoder, place.root);
    encoder.rows(place.projections, [&](const auto& projection) {
        encoder.enumeration(projection.kind);
        encoder.u64(projection.index);
        encodeTableRef(encoder, projection.dynamicIndex);
    });
}

bool decodePlace(Decoder& decoder, PlaceRef& place) {
    if (!decodeTableRef(decoder, place.root)) return false;
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        PlaceProjection projection;
        if (!decoder.enumeration(projection.kind, 3) ||
            !decoder.u64(projection.index) ||
            !decodeTableRef(decoder, projection.dynamicIndex)) return false;
        place.projections.push_back(projection);
    }
    return true;
}

void encodeEdge(Encoder& encoder, const ControlEdge& edge) {
    encodeTableRef(encoder, edge.target);
    encodeTableRefs(encoder, edge.cleanups);
}

bool decodeEdge(Decoder& decoder, ControlEdge& edge) {
    return decodeTableRef(decoder, edge.target) &&
        decodeTableRefs(decoder, edge.cleanups);
}

bool encodeOperation(Encoder& encoder, const Stmt& operation, uint32_t depth,
                     const ContainerLimits& limits) {
    const auto opcode = codeOperationOpcode(operation);
    if (!opcode)
        return encoder.reject(
            "Moon Container sealed CFG contains a structured operation");
    encoder.enumeration(*opcode);
    encodeLocation(encoder, operation.location);
    if (const auto* value = dynamic_cast<const LetStmt*>(&operation)) {
        encoder.string(value->name);
        encodeTableRef(encoder, value->local);
        encoder.boolean(value->isConst);
        encoder.boolean(value->isLinear);
        encoder.enumeration(value->usage);
        encoder.boolean(value->relation.has_value());
        if (value->relation) encoder.enumeration(*value->relation);
        encoder.string(value->type.value);
        if (!encodeOptionalExpr(
                encoder, value->initializer.get(), depth, limits)) return false;
        encoder.boolean(value->materializesIteratorRecipe);
        encoder.boolean(value->materializedIteratorOwnsSource);
        encoder.string(value->materializedIteratorSourceType.value);
    } else if (const auto* value = dynamic_cast<const AllocateStmt*>(&operation)) {
        encodeTableRef(encoder, value->local);
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
    } else if (const auto* value = dynamic_cast<const ExprStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->expr.get(), depth, limits)) return false;
    } else if (const auto* value = dynamic_cast<const FreeStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), depth, limits)) return false;
        encoder.enumeration(value->action);
        encoder.boolean(value->isImplicit);
    } else if (const auto* value = dynamic_cast<const AwaitStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->event.get(), depth, limits)) return false;
    }
    return encoder.good();
}

bool decodeOperation(Decoder& decoder, std::unique_ptr<Stmt>& operation,
                     uint32_t depth, const ContainerLimits& limits) {
    CodeOperationOpcode opcode;
    if (!decoder.enumeration(
            opcode, static_cast<uint32_t>(CodeOperationOpcode::Await)) ||
        static_cast<uint32_t>(opcode) == 0)
        return false;
    std::unique_ptr<Stmt> decoded;
    switch (opcode) {
        case CodeOperationOpcode::Let: decoded = std::make_unique<LetStmt>(); break;
        case CodeOperationOpcode::Allocate: decoded = std::make_unique<AllocateStmt>(); break;
        case CodeOperationOpcode::Expression: decoded = std::make_unique<ExprStmt>(); break;
        case CodeOperationOpcode::Free: decoded = std::make_unique<FreeStmt>(); break;
        case CodeOperationOpcode::Await: decoded = std::make_unique<AwaitStmt>(); break;
    }
    if (!decoded || !decodeLocation(decoder, decoded->location)) return false;
    if (auto* value = dynamic_cast<LetStmt*>(decoded.get())) {
        if (!decoder.string(value->name) ||
            !decodeTableRef(decoder, value->local) ||
            !decoder.boolean(value->isConst) ||
            !decoder.boolean(value->isLinear) ||
            !decoder.enumeration(value->usage, 2)) return false;
        bool hasRelation = false;
        if (!decoder.boolean(hasRelation)) return false;
        if (hasRelation) {
            luna::ownership::Relation relation;
            if (!decoder.enumeration(relation, 2)) return false;
            value->relation = relation;
        }
        if (!decoder.string(value->type.value) ||
            !decodeOptionalExpr(decoder, value->initializer, depth, limits) ||
            !decoder.boolean(value->materializesIteratorRecipe) ||
            !decoder.boolean(value->materializedIteratorOwnsSource) ||
            !decoder.string(value->materializedIteratorSourceType.value)) return false;
    } else if (auto* value = dynamic_cast<AllocateStmt*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->local) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
    } else if (auto* value = dynamic_cast<ExprStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->expr, depth, limits)) return false;
    } else if (auto* value = dynamic_cast<FreeStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, depth, limits) ||
            !decoder.enumeration(value->action, 7) ||
            !decoder.boolean(value->isImplicit)) return false;
    } else if (auto* value = dynamic_cast<AwaitStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->event, depth, limits)) return false;
    }
    operation = std::move(decoded);
    return true;
}

bool encodeTerminator(Encoder& encoder, const Terminator& terminator,
                      uint32_t depth, const ContainerLimits& limits) {
    encoder.enumeration(terminator.kind);
    encodeLocation(encoder, terminator.location);
    if (!encodeOptionalExpr(
            encoder, terminator.operand.get(), depth, limits)) return false;
    encoder.string(terminator.switchType.value);
    encodeEdge(encoder, terminator.primary);
    encodeEdge(encoder, terminator.secondary);
    encoder.rows(terminator.cases, [&](const auto& switchCase) {
        encoder.u32(switchCase.tag);
        encodeEdge(encoder, switchCase.edge);
        encodeTableRefs(encoder, switchCase.bindings);
    });
    encodeTableRefs(encoder, terminator.exitCleanups);
    return encoder.good();
}

bool decodeTerminator(Decoder& decoder, Terminator& terminator,
                      uint32_t depth, const ContainerLimits& limits) {
    if (!decoder.enumeration(terminator.kind, 7) ||
        !decodeLocation(decoder, terminator.location) ||
        !decodeOptionalExpr(decoder, terminator.operand, depth, limits) ||
        !decoder.string(terminator.switchType.value) ||
        !decodeEdge(decoder, terminator.primary) ||
        !decodeEdge(decoder, terminator.secondary)) return false;
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        SwitchEdge switchCase;
        if (!decoder.u32(switchCase.tag) ||
            !decodeEdge(decoder, switchCase.edge) ||
            !decodeTableRefs(decoder, switchCase.bindings)) return false;
        terminator.cases.push_back(std::move(switchCase));
    }
    return decodeTableRefs(decoder, terminator.exitCleanups);
}

bool encodeGraph(Encoder& encoder, const ControlFlowGraph& graph,
                 uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return encoder.reject("Moon Container code exceeds the nesting limit");
    if (!graph.sealed)
        return encoder.reject("Moon Container code contains an unsealed CFG");
    encoder.boolean(graph.sealed);
    encodeTableRef(encoder, graph.entry);
    encodeTableRef(encoder, graph.rootRegion);
    encodeTableRef(encoder, graph.rootScope);
    encoder.rows(graph.blocks, [&](const auto& block) {
        encodeTableRef(encoder, block.id);
        encodeTableRef(encoder, block.region);
        encodeTableRef(encoder, block.scope);
        encodeLocation(encoder, block.location);
        encoder.rows(block.operations, [&](const auto& operation) {
            if (!operation) {
                encoder.reject("Moon Container CFG contains a null operation");
            } else if (encoder.good()) {
                encodeOperation(encoder, *operation, depth, limits);
            }
        });
        if (encoder.good()) encodeTerminator(
            encoder, block.terminator, depth, limits);
    });
    encoder.rows(graph.regions, [&](const auto& region) {
        encodeTableRef(encoder, region.id);
        encodeTableRef(encoder, region.parent);
        encoder.enumeration(region.kind);
        encodeTableRef(encoder, region.scope);
        encodeTableRef(encoder, region.entry);
        encodeTableRef(encoder, region.exit);
        encodeTableRefs(encoder, region.blocks);
        encodeLocation(encoder, region.location);
        encodeReference(encoder, region.fragment);
        encodeTableRefs(encoder, region.parameters);
    });
    encoder.rows(graph.scopes, [&](const auto& scope) {
        encodeTableRef(encoder, scope.id);
        encodeTableRef(encoder, scope.parent);
        encodeTableRef(encoder, scope.region);
        encodeTableRefs(encoder, scope.locals);
        encodeTableRefs(encoder, scope.cleanups);
        encodeLocation(encoder, scope.location);
    });
    encoder.rows(graph.locals, [&](const auto& local) {
        encodeTableRef(encoder, local.id);
        encodeTableRef(encoder, local.scope);
        encoder.enumeration(local.kind);
        encoder.string(local.name);
        encoder.string(local.type.value);
        encoder.enumeration(local.usage);
        encoder.enumeration(local.relation);
    });
    encoder.rows(graph.cleanups, [&](const auto& cleanup) {
        encodeTableRef(encoder, cleanup.id);
        encodeTableRef(encoder, cleanup.scope);
        encodePlace(encoder, cleanup.place);
        encoder.string(cleanup.type.value);
        encoder.enumeration(cleanup.kind);
        encoder.enumeration(cleanup.action);
        encoder.boolean(cleanup.guard.has_value());
        if (cleanup.guard) {
            encodeTableRef(encoder, cleanup.guard->nextUnread);
            encoder.u64(cleanup.guard->elementIndex);
        }
    });
    return encoder.good();
}

bool decodeGraph(Decoder& decoder, std::unique_ptr<ControlFlowGraph>& graph,
                 uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return decoder.reject("Moon Container code exceeds the nesting limit");
    auto decoded = std::make_unique<ControlFlowGraph>();
    if (!decoder.boolean(decoded->sealed) || !decoded->sealed ||
        !decodeTableRef(decoder, decoded->entry) ||
        !decodeTableRef(decoder, decoded->rootRegion) ||
        !decodeTableRef(decoder, decoded->rootScope))
        return decoder.reject("Moon Container code contains an unsealed or truncated CFG");
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        BasicBlock block;
        if (!decodeTableRef(decoder, block.id) || block.id.value != index ||
            !decodeTableRef(decoder, block.region) ||
            !decodeTableRef(decoder, block.scope) ||
            !decodeLocation(decoder, block.location))
            return decoder.reject("Moon Container block table is not canonical");
        uint32_t operationCount = 0;
        if (!decoder.rowCount(operationCount)) return false;
        for (uint32_t operationIndex = 0;
             operationIndex < operationCount; ++operationIndex) {
            std::unique_ptr<Stmt> operation;
            if (!decodeOperation(
                    decoder, operation, depth, limits)) return false;
            block.operations.push_back(std::move(operation));
        }
        if (!decodeTerminator(
                decoder, block.terminator, depth, limits)) return false;
        decoded->blocks.push_back(std::move(block));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        RegionRecord region;
        if (!decodeTableRef(decoder, region.id) || region.id.value != index ||
            !decodeTableRef(decoder, region.parent) ||
            !decoder.enumeration(region.kind, 7) ||
            !decodeTableRef(decoder, region.scope) ||
            !decodeTableRef(decoder, region.entry) ||
            !decodeTableRef(decoder, region.exit) ||
            !decodeTableRefs(decoder, region.blocks) ||
            !decodeLocation(decoder, region.location) ||
            !decodeReference(decoder, region.fragment) ||
            !decodeTableRefs(decoder, region.parameters))
            return decoder.reject("Moon Container region table is not canonical");
        decoded->regions.push_back(std::move(region));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        ScopeRecord scope;
        if (!decodeTableRef(decoder, scope.id) || scope.id.value != index ||
            !decodeTableRef(decoder, scope.parent) ||
            !decodeTableRef(decoder, scope.region) ||
            !decodeTableRefs(decoder, scope.locals) ||
            !decodeTableRefs(decoder, scope.cleanups) ||
            !decodeLocation(decoder, scope.location))
            return decoder.reject("Moon Container scope table is not canonical");
        decoded->scopes.push_back(std::move(scope));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        LocalRecord local;
        if (!decodeTableRef(decoder, local.id) || local.id.value != index ||
            !decodeTableRef(decoder, local.scope) ||
            !decoder.enumeration(local.kind, 4) ||
            !decoder.string(local.name) ||
            !decoder.string(local.type.value) ||
            !decoder.enumeration(local.usage, 2) ||
            !decoder.enumeration(local.relation, 2))
            return decoder.reject("Moon Container local table is not canonical");
        decoded->locals.push_back(std::move(local));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        CleanupRecord cleanup;
        if (!decodeTableRef(decoder, cleanup.id) || cleanup.id.value != index ||
            !decodeTableRef(decoder, cleanup.scope) ||
            !decodePlace(decoder, cleanup.place) ||
            !decoder.string(cleanup.type.value) ||
            !decoder.enumeration(cleanup.kind, 1) ||
            !decoder.enumeration(cleanup.action, 7))
            return decoder.reject("Moon Container cleanup table is not canonical");
        bool hasGuard = false;
        if (!decoder.boolean(hasGuard)) return false;
        if (hasGuard) {
            CleanupGuard guard;
            if (!decodeTableRef(decoder, guard.nextUnread) ||
                !decoder.u64(guard.elementIndex)) return false;
            cleanup.guard = guard;
        }
        decoded->cleanups.push_back(std::move(cleanup));
    }
    graph = std::move(decoded);
    return true;
}

bool isGenericRecipe(const FunctionDecl& function) {
    return !function.typeParams.empty() && !function.isTemplateInstance;
}

void collectConcreteFunctions(
    const std::vector<std::unique_ptr<Decl>>& declarations,
    std::vector<const FunctionDecl*>& functions) {
    for (const auto& declaration : declarations) {
        if (!declaration) continue;
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (!isGenericRecipe(*function)) functions.push_back(function);
            continue;
        }
        if (const auto* implementation =
                dynamic_cast<const ImplDecl*>(declaration.get())) {
            for (const auto& method : implementation->methods)
                if (method && !isGenericRecipe(*method))
                    functions.push_back(method.get());
        }
    }
}

const DeclarationRecord* findDeclarationRecord(
    const Module& module, const DeclarationRef& reference) {
    for (const auto& declaration : module.declarationTable)
        if (declaration.symbolId == reference.symbol &&
            declaration.contractId == reference.contract)
            return &declaration;
    return nullptr;
}

bool containsGenericRecipe(const Module& module) {
    for (const auto& type : module.typeTable)
        if (type.kind == TypeKind::TypeParam ||
            type.kind == TypeKind::InferenceVar ||
            type.kind == TypeKind::Unknown)
            return true;
    for (const auto& declaration : module.declarations) {
        if (!declaration) continue;
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (isGenericRecipe(*function)) return true;
        } else if (const auto* structure =
                       dynamic_cast<const StructDecl*>(declaration.get())) {
            if (!structure->typeParams.empty()) return true;
        } else if (const auto* enumeration =
                       dynamic_cast<const EnumDecl*>(declaration.get())) {
            if (!enumeration->typeParams.empty()) return true;
        } else if (const auto* trait =
                       dynamic_cast<const TraitDecl*>(declaration.get())) {
            if (!trait->typeParams.empty()) return true;
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration.get())) {
            if (!implementation->typeParams.empty()) return true;
            for (const auto& method : implementation->methods)
                if (method && isGenericRecipe(*method)) return true;
        }
    }
    return false;
}

struct ProjectionReferences {
    std::unordered_set<std::string> types;
    std::unordered_set<std::string> declarations;
    std::unordered_set<std::string> schemas;

    void type(const TypeRef& reference) {
        if (!reference.empty()) types.insert(reference.value);
    }
    void declaration(const DeclarationRef& reference) {
        if (!reference.empty()) declarations.insert(reference.symbol.value);
    }
    void schema(const std::string& id) {
        if (!id.empty()) schemas.insert(id);
    }
};

void collectGraphReferences(
    const ControlFlowGraph& graph, ProjectionReferences& references);

void collectExpressionReferences(
    const Expr* expression, ProjectionReferences& references) {
    if (!expression) return;
    references.type(expression->type);
    if (const auto* identifier =
            dynamic_cast<const IdentifierExpr*>(expression)) {
        references.declaration(identifier->declaration);
    } else if (const auto* binary =
                   dynamic_cast<const BinaryExpr*>(expression)) {
        collectExpressionReferences(binary->lhs.get(), references);
        collectExpressionReferences(binary->rhs.get(), references);
    } else if (const auto* unary =
                   dynamic_cast<const UnaryExpr*>(expression)) {
        collectExpressionReferences(unary->operand.get(), references);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        collectExpressionReferences(call->callee.get(), references);
        for (const auto& argument : call->args)
            collectExpressionReferences(argument.get(), references);
        for (const auto& argument : call->typeArgs) references.type(argument);
        references.declaration(call->calleeRef);
        references.type(call->intrinsicType);
        references.type(call->iteratorInputType);
        references.type(call->iteratorOutputType);
        references.type(call->iteratorRecipeSourceType);
        references.type(call->iteratorCollectTargetType);
        references.type(call->iteratorCollectBuilderType);
        references.declaration(call->iteratorCollectBegin);
        references.declaration(call->iteratorCollectPush);
        references.declaration(call->iteratorCollectFinish);
    } else if (const auto* launch =
                   dynamic_cast<const LaunchExpr*>(expression)) {
        references.declaration(launch->kernelRef);
        collectExpressionReferences(launch->threads.get(), references);
        for (const auto& argument : launch->args)
            collectExpressionReferences(argument.get(), references);
    } else if (const auto* variant =
                   dynamic_cast<const VariantConstructExpr*>(expression)) {
        references.type(variant->constructedType);
        for (const auto& argument : variant->args)
            collectExpressionReferences(argument.get(), references);
    } else if (const auto* result =
                   dynamic_cast<const ResultConstructExpr*>(expression)) {
        collectExpressionReferences(result->payload.get(), references);
    } else if (const auto* field =
                   dynamic_cast<const FieldAccessExpr*>(expression)) {
        collectExpressionReferences(field->object.get(), references);
    } else if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
        collectExpressionReferences(index->object.get(), references);
        collectExpressionReferences(index->index.get(), references);
    } else if (const auto* length =
                   dynamic_cast<const SliceLengthExpr*>(expression)) {
        collectExpressionReferences(length->slice.get(), references);
    } else if (const auto* array =
                   dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        references.type(array->elementType);
        for (const auto& element : array->elements)
            collectExpressionReferences(element.get(), references);
    } else if (const auto* record =
                   dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            collectExpressionReferences(field.value.get(), references);
    } else if (const auto* allocation =
                   dynamic_cast<const HeapAllocExpr*>(expression)) {
        references.type(allocation->allocatedType);
        collectExpressionReferences(allocation->initializer.get(), references);
    } else if (const auto* allocation =
                   dynamic_cast<const InitAllocationExpr*>(expression)) {
        references.type(allocation->allocatedType);
        for (const auto& element : allocation->elements)
            collectExpressionReferences(element.value.get(), references);
    } else if (const auto* move = dynamic_cast<const MoveExpr*>(expression)) {
        collectExpressionReferences(move->operand.get(), references);
    } else if (const auto* borrow =
                   dynamic_cast<const BorrowExpr*>(expression)) {
        collectExpressionReferences(borrow->operand.get(), references);
    } else if (const auto* dereference =
                   dynamic_cast<const DerefExpr*>(expression)) {
        collectExpressionReferences(dereference->operand.get(), references);
    } else if (const auto* address =
                   dynamic_cast<const AddrOfExpr*>(expression)) {
        collectExpressionReferences(address->operand.get(), references);
    } else if (const auto* lambda =
                   dynamic_cast<const LambdaExpr*>(expression)) {
        for (const auto& parameter : lambda->params)
            references.type(parameter.type);
        references.type(lambda->returnType);
        references.type(lambda->closureType);
        if (lambda->controlFlow)
            collectGraphReferences(*lambda->controlFlow, references);
    } else if (const auto* closure =
                   dynamic_cast<const MakeClosureExpr*>(expression)) {
        collectExpressionReferences(closure->lambda.get(), references);
        for (const auto& value : closure->capturedValues)
            collectExpressionReferences(value.get(), references);
    } else if (const auto* assignment =
                   dynamic_cast<const AssignExpr*>(expression)) {
        collectExpressionReferences(assignment->lhs.get(), references);
        collectExpressionReferences(assignment->rhs.get(), references);
    }
}

void collectGraphReferences(
    const ControlFlowGraph& graph, ProjectionReferences& references) {
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (const auto* let = dynamic_cast<const LetStmt*>(operation.get())) {
                references.type(let->type);
                references.type(let->materializedIteratorSourceType);
                collectExpressionReferences(let->initializer.get(), references);
            } else if (const auto* allocation =
                           dynamic_cast<const AllocateStmt*>(operation.get())) {
                references.type(allocation->allocatedType);
            } else if (const auto* statement =
                           dynamic_cast<const ExprStmt*>(operation.get())) {
                collectExpressionReferences(statement->expr.get(), references);
            } else if (const auto* release =
                           dynamic_cast<const FreeStmt*>(operation.get())) {
                collectExpressionReferences(release->operand.get(), references);
            } else if (const auto* await =
                           dynamic_cast<const AwaitStmt*>(operation.get())) {
                collectExpressionReferences(await->event.get(), references);
            }
        }
        references.type(block.terminator.switchType);
        collectExpressionReferences(
            block.terminator.operand.get(), references);
    }
    for (const auto& region : graph.regions)
        references.declaration(region.fragment);
    for (const auto& local : graph.locals) references.type(local.type);
    for (const auto& cleanup : graph.cleanups) references.type(cleanup.type);
}

void collectFunctionReferences(
    const FunctionDecl& function, ProjectionReferences& references) {
    for (const auto& parameter : function.params)
        references.type(parameter.type);
    references.type(function.returnType);
    for (const auto& argument : function.concreteTypeArgs)
        references.type(argument);
    if (function.controlFlow)
        collectGraphReferences(*function.controlFlow, references);
}

bool buildConcreteProjection(
    const ContainerManifest& manifest, const Module& source,
    Module& projection, std::string& error) {
    std::unordered_set<std::string> genericRecipeSymbols;
    for (const auto& declaration : source.declarations) {
        if (!declaration) continue;
        const auto remember = [&](const Decl& recipe) {
            genericRecipeSymbols.insert(recipe.symbolId.value);
        };
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (isGenericRecipe(*function)) remember(*function);
        } else if (const auto* structure =
                       dynamic_cast<const StructDecl*>(declaration.get())) {
            if (!structure->typeParams.empty()) remember(*structure);
        } else if (const auto* enumeration =
                       dynamic_cast<const EnumDecl*>(declaration.get())) {
            if (!enumeration->typeParams.empty()) remember(*enumeration);
        } else if (const auto* trait =
                       dynamic_cast<const TraitDecl*>(declaration.get())) {
            if (!trait->typeParams.empty()) remember(*trait);
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration.get())) {
            if (!implementation->typeParams.empty()) remember(*implementation);
            for (const auto& method : implementation->methods)
                if (method && isGenericRecipe(*method)) remember(*method);
        }
    }

    std::unordered_map<std::string, size_t> typeIndexes;
    typeIndexes.reserve(source.typeTable.size());
    for (size_t index = 0; index < source.typeTable.size(); ++index)
        typeIndexes.emplace(source.typeTable[index].id.value, index);

    std::vector<bool> concrete(source.typeTable.size(), true);
    for (size_t index = 0; index < source.typeTable.size(); ++index) {
        const auto kind = source.typeTable[index].kind;
        if (kind == TypeKind::TypeParam || kind == TypeKind::InferenceVar ||
            kind == TypeKind::Unknown)
            concrete[index] = false;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = 0; index < source.typeTable.size(); ++index) {
            if (!concrete[index]) continue;
            for (const auto& reference :
                 source.typeTable[index].referencedTypeIds) {
                const auto found = typeIndexes.find(reference.value);
                if (found == typeIndexes.end() || !concrete[found->second]) {
                    concrete[index] = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    std::unordered_set<std::string> concreteTypes;
    for (size_t index = 0; index < source.typeTable.size(); ++index) {
        if (!concrete[index]) continue;
        concreteTypes.insert(source.typeTable[index].id.value);
    }
    std::unordered_map<std::string, const DeclarationRecord*>
        concreteDeclarations;
    for (const auto& declaration : source.declarationTable) {
        if (genericRecipeSymbols.count(declaration.symbolId.value) != 0 ||
            declaration.type.empty() ||
            concreteTypes.count(declaration.type.value) == 0)
            continue;
        concreteDeclarations.emplace(declaration.symbolId.value, &declaration);
    }

    std::vector<const FunctionDecl*> concreteFunctions;
    collectConcreteFunctions(source.declarations, concreteFunctions);
    std::unordered_map<std::string, const FunctionDecl*> functionsBySymbol;
    for (const auto* function : concreteFunctions)
        functionsBySymbol.emplace(function->symbolId.value, function);

    std::unordered_map<std::string, const MetadataSchema*> schemasById;
    for (const auto& schema : source.metadataSchemas)
        schemasById.emplace(schema.id, &schema);

    std::unordered_set<std::string> selectedTypes;
    std::unordered_set<std::string> selectedDeclarations;
    std::unordered_set<std::string> selectedSchemas;
    std::vector<std::string> pendingTypes;
    std::vector<std::string> pendingDeclarations;
    std::vector<std::string> pendingSchemas;
    const auto selectType = [&](const TypeRef& reference) {
        if (!reference.empty() && selectedTypes.insert(reference.value).second)
            pendingTypes.push_back(reference.value);
    };
    const auto selectDeclaration = [&](const DeclarationRef& reference) {
        if (!reference.empty() &&
            selectedDeclarations.insert(reference.symbol.value).second)
            pendingDeclarations.push_back(reference.symbol.value);
    };
    const auto selectSchema = [&](const std::string& schema) {
        if (!schema.empty() && selectedSchemas.insert(schema).second)
            pendingSchemas.push_back(schema);
    };

    if (!manifest.entrypoint.empty()) selectDeclaration(manifest.entrypoint);
    for (const auto& exported : source.exports)
        selectDeclaration(exported.declaration);
    for (const auto& import : source.imports)
        if (import.kind == ImportKind::Host)
            selectDeclaration(import.declaration);
    for (const auto& declaration : source.declarationTable) {
        if (declaration.retention != Retention::CompileTime &&
            concreteDeclarations.count(declaration.symbolId.value) != 0)
            selectDeclaration({declaration.symbolId, declaration.contractId});
    }

    size_t typeCursor = 0;
    size_t declarationCursor = 0;
    size_t schemaCursor = 0;
    while (typeCursor < pendingTypes.size() ||
           declarationCursor < pendingDeclarations.size() ||
           schemaCursor < pendingSchemas.size()) {
        while (declarationCursor < pendingDeclarations.size()) {
            const auto symbol = pendingDeclarations[declarationCursor++];
            const auto found = concreteDeclarations.find(symbol);
            if (found == concreteDeclarations.end()) {
                error = "Moon Container reachable declaration is generic or unresolved";
                return false;
            }
            const auto& declaration = *found->second;
            selectType(declaration.type);
            selectDeclaration(declaration.dropGlue);
            if (declaration.kind == DeclarationKind::MetadataSchema)
                selectSchema(declaration.id);
            for (const auto& metadata : declaration.metadata)
                selectSchema(metadata.schemaId);
            const auto function = functionsBySymbol.find(symbol);
            if (function != functionsBySymbol.end()) {
                ProjectionReferences references;
                collectFunctionReferences(*function->second, references);
                for (const auto& type : references.types)
                    selectType(TypeRef{type});
                for (const auto& dependency : references.declarations) {
                    const auto record = concreteDeclarations.find(dependency);
                    if (record == concreteDeclarations.end()) {
                        error = "Moon Container reachable code depends on a generic or missing declaration";
                        return false;
                    }
                    selectDeclaration({record->second->symbolId,
                                       record->second->contractId});
                }
                for (const auto& schema : references.schemas)
                    selectSchema(schema);
            }
        }
        while (typeCursor < pendingTypes.size()) {
            const auto id = pendingTypes[typeCursor++];
            if (concreteTypes.count(id) == 0) {
                error = "Moon Container reachable model depends on a generic or missing type";
                return false;
            }
            const auto found = typeIndexes.find(id);
            if (found == typeIndexes.end()) {
                error = "Moon Container reachable TypeId is absent from the frozen table";
                return false;
            }
            const auto& type = source.typeTable[found->second];
            for (const auto& dependency : type.referencedTypeIds)
                selectType(TypeRef{dependency.value});
            selectDeclaration(type.dropGlue);
        }
        while (schemaCursor < pendingSchemas.size()) {
            const auto id = pendingSchemas[schemaCursor++];
            const auto found = schemasById.find(id);
            if (found == schemasById.end()) {
                error = "Moon Container reachable declaration uses a missing metadata schema";
                return false;
            }
            for (const auto& field : found->second->fields)
                selectType(field.type);
        }
    }

    for (const auto& type : source.typeTable)
        if (selectedTypes.count(type.id.value) != 0)
            projection.typeTable.push_back(type);
    projection.typeTableSealed = source.typeTableSealed;
    for (const auto& declaration : source.declarationTable)
        if (selectedDeclarations.count(declaration.symbolId.value) != 0)
            projection.declarationTable.push_back(declaration);
    for (const auto& schema : source.metadataSchemas)
        if (selectedSchemas.count(schema.id) != 0)
            projection.metadataSchemas.push_back(schema);

    for (const auto& import : source.imports) {
        if (import.kind == ImportKind::Package ||
            selectedDeclarations.count(import.declaration.symbol.value) != 0)
            projection.imports.push_back(import);
    }
    projection.exports = source.exports;
    for (const auto& exported : projection.exports) {
        if (selectedDeclarations.count(exported.declaration.symbol.value) == 0 ||
            selectedTypes.count(exported.type.value) == 0) {
            error = "Moon Container export has a generic contract";
            return false;
        }
    }

    projection.formatMajor = source.formatMajor;
    projection.formatMinor = source.formatMinor;
    projection.name = source.name;
    projection.packageUses = source.packageUses;
    projection.isPackage = source.isPackage;
    projection.features = source.features;
    projection.rebuildIndexes();
    return true;
}

bool encodeFunction(
    Encoder& encoder, const FunctionDecl& function,
    const ContainerLimits& limits) {
    encodeReference(encoder, {function.symbolId, function.contractId});
    encoder.string(function.packageId);
    encoder.string(function.modulePath);
    encoder.string(function.name);
    encoder.string(function.generatedSymbolName);
    encoder.boolean(function.isKernel);
    encoder.boolean(function.isCodegenReachable);
    encoder.boolean(function.isExtern);
    encoder.boolean(function.isConstexpr);
    encoder.boolean(function.isSelector);
    encoder.string(function.abi);
    encoder.string(function.linkName);
    encoder.rows(function.typeParams, [&](const auto& parameter) {
        encoder.string(parameter);
    });
    encoder.rows(function.params, [&](const auto& parameter) {
        encodeParam(encoder, parameter);
    });
    encoder.string(function.returnType.value);
    encoder.boolean(function.returnsLinear);
    encoder.enumeration(function.returnUsage);
    encoder.boolean(function.isTemplateInstance);
    encoder.rows(function.concreteTypeArgs, [&](const auto& argument) {
        encoder.string(argument.value);
    });
    encodeLocation(encoder, function.location);
    encoder.boolean(function.controlFlow != nullptr);
    return !function.controlFlow ||
        encodeGraph(encoder, *function.controlFlow, 0, limits);
}

bool decodeFunction(
    Decoder& decoder, std::unique_ptr<FunctionDecl>& function,
    DeclarationRef& reference, const ContainerLimits& limits) {
    auto decoded = std::make_unique<FunctionDecl>();
    if (!decodeReference(decoder, reference) ||
        !decoder.string(decoded->packageId) ||
        !decoder.string(decoded->modulePath) ||
        !decoder.string(decoded->name) ||
        !decoder.string(decoded->generatedSymbolName) ||
        !decoder.boolean(decoded->isKernel) ||
        !decoder.boolean(decoded->isCodegenReachable) ||
        !decoder.boolean(decoded->isExtern) ||
        !decoder.boolean(decoded->isConstexpr) ||
        !decoder.boolean(decoded->isSelector) ||
        !decoder.string(decoded->abi) ||
        !decoder.string(decoded->linkName)) return false;

    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        std::string parameter;
        if (!decoder.string(parameter)) return false;
        decoded->typeParams.push_back(std::move(parameter));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        Param parameter;
        if (!decodeParam(decoder, parameter)) return false;
        decoded->params.push_back(std::move(parameter));
    }
    if (!decoder.string(decoded->returnType.value) ||
        !decoder.boolean(decoded->returnsLinear) ||
        !decoder.enumeration(decoded->returnUsage, 2) ||
        !decoder.boolean(decoded->isTemplateInstance) ||
        !decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        TypeRef argument;
        if (!decoder.string(argument.value)) return false;
        decoded->concreteTypeArgs.push_back(std::move(argument));
    }
    if (!decodeLocation(decoder, decoded->location)) return false;
    bool hasGraph = false;
    if (!decoder.boolean(hasGraph)) return false;
    if (hasGraph &&
        !decodeGraph(decoder, decoded->controlFlow, 0, limits)) return false;
    function = std::move(decoded);
    return true;
}

} // namespace

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

bool encodeCodeRows(
    const Module& source, const Module& declarationModel,
    std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    std::vector<const FunctionDecl*> functions;
    collectConcreteFunctions(source.declarations, functions);
    functions.erase(std::remove_if(
        functions.begin(), functions.end(), [&](const auto* function) {
            return !findDeclarationRecord(
                declarationModel,
                {function->symbolId, function->contractId});
        }), functions.end());
    std::sort(functions.begin(), functions.end(), [](const auto* left,
                                                     const auto* right) {
        return left->symbolId.value < right->symbolId.value;
    });

    std::string previousSymbol;
    for (const auto* function : functions) {
        if (function->symbolId.empty() || function->contractId.empty()) {
            error = "Moon Container function has no complete declaration reference";
            output.clear(); return false;
        }
        if (!previousSymbol.empty() &&
            function->symbolId.value <= previousSymbol) {
            error = "Moon Container functions have duplicate SymbolIds";
            output.clear(); return false;
        }
        previousSymbol = function->symbolId.value;
        const auto* declaration = findDeclarationRecord(
            declarationModel, {function->symbolId, function->contractId});
        if (!declaration || declaration->kind != DeclarationKind::Function ||
            declaration->id != function->declarationId ||
            declaration->familyId != function->familyId ||
            declaration->sourceName != function->name ||
            declaration->linkageName != function->generatedSymbolName) {
            error = "Moon Container function disagrees with its declaration row";
            output.clear(); return false;
        }
        if (function->body) {
            error = "Moon Container code cannot contain a structured function body";
            output.clear(); return false;
        }
        if (function->isExtern == static_cast<bool>(function->controlFlow)) {
            error = function->isExtern
                ? "Moon Container extern function carries a CFG"
                : "Moon Container concrete function has no CFG";
            output.clear(); return false;
        }
        if (function->controlFlow && !function->controlFlow->sealed) {
            error = "Moon Container function carries an unsealed CFG";
            output.clear(); return false;
        }
    }

    Encoder encoder(limits);
    encoder.rows(functions, [&](const auto* function) {
        if (encoder.good()) encodeFunction(encoder, *function, limits);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::encodeCode(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    return encodeCodeRows(module, module, output, error, limits);
}

bool ContainerModelCodec::decodeCode(
    const std::vector<uint8_t>& input, Module& module,
    std::string& error, const ContainerLimits& limits) {
    if (input.size() > limits.maximumContainerBytes) {
        error = "Moon Container code payload exceeds the configured byte limit";
        return false;
    }
    Decoder decoder(input, limits);
    uint32_t count = 0;
    if (!decoder.rowCount(count)) {
        error = decoder.error(); return false;
    }

    std::vector<std::unique_ptr<Decl>> declarations;
    std::string previousSymbol;
    for (uint32_t index = 0; index < count; ++index) {
        std::unique_ptr<FunctionDecl> function;
        DeclarationRef reference;
        if (!decodeFunction(decoder, function, reference, limits)) {
            error = decoder.error().empty()
                ? "Moon Container function row contains an invalid scalar"
                : decoder.error();
            return false;
        }
        if (!reference.complete() ||
            (!previousSymbol.empty() &&
             reference.symbol.value <= previousSymbol)) {
            error = "Moon Container functions are incomplete, duplicate, or out of order";
            return false;
        }
        previousSymbol = reference.symbol.value;
        const auto* declaration = findDeclarationRecord(module, reference);
        if (!declaration || declaration->kind != DeclarationKind::Function ||
            declaration->sourceName != function->name ||
            declaration->linkageName != function->generatedSymbolName) {
            error = "Moon Container function does not match its declaration row";
            return false;
        }
        if (isGenericRecipe(*function)) {
            error = "Moon Container code contains a generic function recipe";
            return false;
        }
        if (function->isExtern == static_cast<bool>(function->controlFlow)) {
            error = function->isExtern
                ? "Moon Container extern function carries a CFG"
                : "Moon Container concrete function has no CFG";
            return false;
        }
        function->declarationId = declaration->id;
        function->familyId = declaration->familyId;
        function->symbolId = declaration->symbolId;
        function->contractId = declaration->contractId;
        function->retention = declaration->retention;
        function->metadata = declaration->metadata;
        function->sysmeta = declaration->sysmeta;
        function->isExported = std::any_of(
            module.exports.begin(), module.exports.end(),
            [&](const auto& exported) {
                return exported.declaration == reference;
            });
        declarations.push_back(std::move(function));
    }
    if (!decoder.finish("code")) {
        error = decoder.error(); return false;
    }

    module.declarations = std::move(declarations);
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::encodeContainer(
    const ContainerManifest& manifest, const Module& module,
    std::vector<uint8_t>& output, std::string& error,
    const ContainerLimits& limits) {
    output.clear();
    if (module.name != manifest.packageId ||
        featureBits(module.features) != featureBits(manifest.features)) {
        error = "Moon Container manifest disagrees with its MoonIR module";
        return false;
    }
    Verifier verifier;
    if (!verifier.verify(module)) {
        error = verifier.errors().empty()
            ? "Moon Container MoonIR verification failed"
            : "Moon Container MoonIR verification failed: " +
                verifier.errors().front().message;
        return false;
    }

    Module projection;
    if (!buildConcreteProjection(
            manifest, module, projection, error)) return false;
    const auto* entry = manifest.entrypoint.empty()
        ? nullptr : findDeclarationRecord(projection, manifest.entrypoint);
    if ((manifest.packageKind == ContainerPackageKind::Application &&
         (!entry || entry->kind != DeclarationKind::Function)) ||
        (manifest.packageKind == ContainerPackageKind::Library && entry)) {
        error = "Moon Container entrypoint does not survive the concrete projection; "
                "generic recipes are not executable container entries";
        return false;
    }
    Verifier projectionVerifier;
    if (!projectionVerifier.verify(projection)) {
        error = projectionVerifier.errors().empty()
            ? "Moon Container concrete projection verification failed"
            : "Moon Container concrete projection verification failed: " +
                projectionVerifier.errors().front().message;
        return false;
    }

    std::vector<ContainerSection> sections;
    const auto append = [&](ContainerSectionId id,
                            auto encode) -> bool {
        ContainerSection section;
        section.id = static_cast<uint32_t>(id);
        if (!encode(section.payload)) return false;
        sections.push_back(std::move(section));
        return true;
    };
    if (!append(ContainerSectionId::Manifest, [&](auto& payload) {
            return encodeManifest(manifest, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Type, [&](auto& payload) {
            return encodeTypes(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Symbol, [&](auto& payload) {
            return encodeSymbols(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Contract, [&](auto& payload) {
            return encodeContracts(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Code, [&](auto& payload) {
            return encodeCodeRows(
                module, projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Imports, [&](auto& payload) {
            return encodeImports(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Exports, [&](auto& payload) {
            return encodeExports(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Sysmeta, [&](auto& payload) {
            return encodeSysmeta(projection, payload, error, limits);
        })) {
        output.clear();
        return false;
    }
    return ContainerWriter::encode(
        std::move(sections), output, error, limits);
}

bool ContainerModelCodec::decodeContainer(
    const std::vector<uint8_t>& input, ContainerManifest& manifest,
    Module& module, std::string& error, const ContainerLimits& limits) {
    ContainerReader reader;
    if (!reader.parse(input, error, limits)) return false;
    const auto section = [&](ContainerSectionId id)
        -> const std::vector<uint8_t>* {
        const auto* found = reader.find(static_cast<uint32_t>(id));
        return found ? &found->payload : nullptr;
    };
    const auto* manifestBytes = section(ContainerSectionId::Manifest);
    const auto* typeBytes = section(ContainerSectionId::Type);
    const auto* symbolBytes = section(ContainerSectionId::Symbol);
    const auto* contractBytes = section(ContainerSectionId::Contract);
    const auto* codeBytes = section(ContainerSectionId::Code);
    const auto* importBytes = section(ContainerSectionId::Imports);
    const auto* exportBytes = section(ContainerSectionId::Exports);
    const auto* sysmetaBytes = section(ContainerSectionId::Sysmeta);
    if (!manifestBytes || !typeBytes || !symbolBytes || !contractBytes ||
        !codeBytes || !importBytes || !exportBytes || !sysmetaBytes) {
        error = "Moon Container is missing a required model section";
        return false;
    }

    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeManifest(
            *manifestBytes, decodedManifest, error, limits)) return false;
    decodedModule.formatMajor = reader.formatMajor();
    decodedModule.formatMinor = reader.formatMinor();
    decodedModule.name = decodedManifest.packageId;
    decodedModule.isPackage = true;
    decodedModule.features = decodedManifest.features;
    if (!decodeTypes(*typeBytes, decodedModule, error, limits) ||
        !decodeDeclarations(
            *symbolBytes, *contractBytes, *sysmetaBytes,
            decodedModule, error, limits) ||
        !decodeInterfaces(
            *importBytes, *exportBytes, decodedModule, error, limits) ||
        !decodeCode(*codeBytes, decodedModule, error, limits)) return false;
    Module decodedProjection;
    if (!buildConcreteProjection(
            decodedManifest, decodedModule, decodedProjection, error))
        return false;
    if (containsGenericRecipe(decodedModule)) {
        error = "Moon Container cannot carry a generic or unresolved type recipe";
        return false;
    }

    const auto* entry = decodedManifest.entrypoint.empty()
        ? nullptr
        : findDeclarationRecord(decodedModule, decodedManifest.entrypoint);
    if ((decodedManifest.packageKind == ContainerPackageKind::Application &&
         (!entry || entry->kind != DeclarationKind::Function)) ||
        (decodedManifest.packageKind == ContainerPackageKind::Library && entry)) {
        error = "Moon Container entrypoint does not match its declaration table";
        return false;
    }
    if (entry) {
        const bool executable = std::any_of(
            decodedModule.declarations.begin(),
            decodedModule.declarations.end(), [&](const auto& declaration) {
                const auto* function = dynamic_cast<const FunctionDecl*>(
                    declaration.get());
                return function &&
                    function->symbolId == decodedManifest.entrypoint.symbol &&
                    function->contractId == decodedManifest.entrypoint.contract &&
                    !function->isExtern && function->controlFlow;
            });
        if (!executable) {
            error = "Moon Container application entrypoint is not executable";
            return false;
        }
    }
    Verifier verifier;
    if (!verifier.verify(decodedModule)) {
        error = verifier.errors().empty()
            ? "Moon Container MoonIR verification failed"
            : "Moon Container MoonIR verification failed: " +
                verifier.errors().front().message;
        return false;
    }

    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::decodeContainerForTarget(
    const std::vector<uint8_t>& input,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    ContainerManifest& manifest, Module& module,
    std::string& error, const ContainerLimits& limits) {
    if (expectedTargetTriple.empty() || expectedDataLayout.empty()) {
        error = "Moon Container executable target expectation is incomplete";
        return false;
    }
    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeContainer(
            input, decodedManifest, decodedModule, error, limits)) return false;
    if (decodedManifest.targetTriple != expectedTargetTriple ||
        decodedManifest.dataLayout != expectedDataLayout) {
        error = "Moon Container target triple or data layout does not match the host";
        return false;
    }
    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}

} // namespace moon
