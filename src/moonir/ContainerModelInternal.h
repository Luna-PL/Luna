#pragma once

#include "ContainerModel.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace moon::container_detail {


inline bool isValidUtf8(const uint8_t* bytes, size_t size) {
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

void encodeReference(Encoder& encoder, const DeclarationRef& reference);
bool decodeReference(Decoder& decoder, DeclarationRef& reference);
template <typename Reference>
void encodeTypeRefs(
    Encoder& encoder, const std::vector<Reference>& references) {
    encoder.rows(references, [&](const auto& reference) {
        encoder.string(reference.value);
    });
}

template <typename Reference>
bool decodeTypeRefs(
    Decoder& decoder, std::vector<Reference>& references) {
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
bool decodeStringRows(Decoder& decoder, std::vector<std::string>& values);
uint32_t featureBits(const FeatureFlags& features);
void encodeLocation(Encoder& encoder, const SourceLocation& location);
bool decodeLocation(Decoder& decoder, SourceLocation& location);
void encodeConstant(Encoder& encoder, const ConstantValue& value);
bool decodeConstant(Decoder& decoder, ConstantValue& value);

bool isGenericRecipe(const FunctionDecl& function);
void collectConcreteFunctions(
    const std::vector<std::unique_ptr<Decl>>& declarations,
    std::vector<const FunctionDecl*>& functions);
const DeclarationRecord* findDeclarationRecord(
    const Module& module, const DeclarationRef& reference);
bool containsGenericRecipe(const Module& module);
bool buildConcreteProjection(
    const ContainerManifest& manifest, const Module& source,
    Module& projection, std::string& error);
bool encodeCodeRows(
    const Module& source, const Module& declarationModel,
    std::vector<uint8_t>& output, std::string& error,
    const ContainerLimits& limits);

} // namespace moon::container_detail
