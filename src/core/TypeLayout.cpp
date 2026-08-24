#include "TypeLayout.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace luna::layout {

uint64_t alignTo(uint64_t value, uint64_t alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

namespace {

uint64_t valueSizeImpl(const TypePtr& type,
                       std::unordered_set<const Type*>& active);

uint64_t valueAlignmentImpl(const TypePtr& type,
                            std::unordered_set<const Type*>& active) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool:
            return 1;
        case TypeKind::I16: case TypeKind::U16:
            return 2;
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
        case TypeKind::Event:
            return 4;
        case TypeKind::Array:
            return valueAlignmentImpl(type->inner, active);
        case TypeKind::Record: {
            uint64_t alignment = 1;
            for (const auto& field : type->fields)
                alignment = std::max(
                    alignment, valueAlignmentImpl(field.type, active));
            return alignment;
        }
        case TypeKind::Closure: {
            uint64_t alignment = 8;
            for (const auto& field : type->capturedFields)
                alignment = std::max(
                    alignment, valueAlignmentImpl(field.type, active));
            return alignment;
        }
        case TypeKind::Unit: case TypeKind::Never:
            return 1;
        case TypeKind::Slice: case TypeKind::Result: case TypeKind::Enum:
            return InlinePayloadAlignment;
        default:
            return 8;
    }
}

uint64_t variantPayloadSizeImpl(
    const TypeVariant& variant, std::unordered_set<const Type*>& active) {
    uint64_t offset = 0;
    uint64_t maximumAlignment = 1;
    for (const auto& field : variant.fields) {
        const uint64_t alignment = std::min<uint64_t>(
            InlinePayloadAlignment, valueAlignmentImpl(field, active));
        maximumAlignment = std::max(maximumAlignment, alignment);
        offset = alignTo(offset, alignment);
        offset += valueSizeImpl(field, active);
    }
    return alignTo(offset, maximumAlignment);
}

uint64_t valueSizeImpl(const TypePtr& type,
                       std::unordered_set<const Type*>& active) {
    if (!type) return 0;
    switch (type->kind) {
        case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool:
            return 1;
        case TypeKind::I16: case TypeKind::U16:
            return 2;
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
        case TypeKind::Event:
            return 4;
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::USize: case TypeKind::ISize: case TypeKind::F64:
        case TypeKind::String: case TypeKind::CStr:
        case TypeKind::RawPointer: case TypeKind::Reference:
        case TypeKind::Struct:
        case TypeKind::DeviceBuffer: case TypeKind::Iterator:
        case TypeKind::Metadata: case TypeKind::MetadataView:
        case TypeKind::SymbolSet: case TypeKind::DeclarationView: case TypeKind::DeclarationRef:
        case TypeKind::Function:
            return 8;
        case TypeKind::Record: {
            if (!active.insert(type.get()).second)
                return 0;
            uint64_t offset = 0;
            uint64_t maximumAlignment = 1;
            for (const auto& field : type->fields) {
                const uint64_t alignment =
                    valueAlignmentImpl(field.type, active);
                maximumAlignment = std::max(maximumAlignment, alignment);
                offset = alignTo(offset, alignment);
                offset += valueSizeImpl(field.type, active);
            }
            active.erase(type.get());
            return alignTo(offset, maximumAlignment);
        }
        case TypeKind::Closure: {
            if (!active.insert(type.get()).second)
                return 0;
            uint64_t offset = 8;
            uint64_t maximumAlignment = 8;
            for (const auto& field : type->capturedFields) {
                const uint64_t alignment =
                    valueAlignmentImpl(field.type, active);
                maximumAlignment = std::max(maximumAlignment, alignment);
                offset = alignTo(offset, alignment);
                offset += valueSizeImpl(field.type, active);
            }
            active.erase(type.get());
            return alignTo(offset, maximumAlignment);
        }
        case TypeKind::Unit: case TypeKind::Never:
            return 0;
        case TypeKind::Slice:
            return 16;
        case TypeKind::Array:
            return type->arrayLength * valueSizeImpl(type->inner, active);
        case TypeKind::Result: {
            uint64_t payload = 0;
            for (const auto& argument : type->typeArgs)
                payload = std::max(payload, valueSizeImpl(argument, active));
            payload = std::max<uint64_t>(
                InlinePayloadAlignment,
                alignTo(payload, InlinePayloadAlignment));
            return InlineTagStorageSize + payload;
        }
        case TypeKind::Enum: {
            if (!active.insert(type.get()).second)
                return 0; // rejected by Sema as an infinite inline layout
            uint64_t payload = 0;
            for (const auto& variant : type->variants)
                payload = std::max(
                    payload, variantPayloadSizeImpl(variant, active));
            active.erase(type.get());
            payload = std::max<uint64_t>(
                InlinePayloadAlignment,
                alignTo(payload, InlinePayloadAlignment));
            return InlineTagStorageSize + payload;
        }
        default:
            return 0;
    }
}

} // namespace

uint64_t valueSize(const TypePtr& type) {
    std::unordered_set<const Type*> active;
    return valueSizeImpl(type, active);
}

uint64_t valueAlignment(const TypePtr& type) {
    std::unordered_set<const Type*> active;
    return valueAlignmentImpl(type, active);
}

uint64_t productStorageAlignment(const TypePtr& type) {
    if (!type || type->kind != TypeKind::Struct) return 1;
    uint64_t alignment = 1;
    for (const auto& field : type->fields)
        alignment = std::max(alignment, valueAlignment(field.type));
    return alignment;
}

uint64_t productFieldOffset(const TypePtr& type, size_t fieldIndex) {
    if (!type || type->kind != TypeKind::Struct) return 0;
    uint64_t offset = 0;
    for (size_t index = 0;
         index < fieldIndex && index < type->fields.size(); ++index) {
        offset = alignTo(
            offset, valueAlignment(type->fields[index].type));
        offset += valueSize(type->fields[index].type);
    }
    if (fieldIndex < type->fields.size())
        offset = alignTo(
            offset, valueAlignment(type->fields[fieldIndex].type));
    return offset;
}

uint64_t productStorageSize(const TypePtr& type) {
    if (!type || type->kind != TypeKind::Struct) return 0;
    if (type->fields.empty()) return 1;
    const size_t last = type->fields.size() - 1;
    const uint64_t end = productFieldOffset(type, last) +
        valueSize(type->fields[last].type);
    return alignTo(end, productStorageAlignment(type));
}

uint64_t variantFieldOffset(const TypeVariant& variant, size_t fieldIndex) {
    std::unordered_set<const Type*> active;
    uint64_t offset = 0;
    for (size_t index = 0;
         index < fieldIndex && index < variant.fields.size(); ++index) {
        const auto& field = variant.fields[index];
        offset = alignTo(
            offset, std::min<uint64_t>(
                InlinePayloadAlignment,
                valueAlignmentImpl(field, active)));
        offset += valueSizeImpl(field, active);
    }
    if (fieldIndex < variant.fields.size())
        offset = alignTo(
            offset, std::min<uint64_t>(
                InlinePayloadAlignment,
                valueAlignmentImpl(variant.fields[fieldIndex], active)));
    return offset;
}

uint64_t variantPayloadSize(const TypeVariant& variant) {
    std::unordered_set<const Type*> active;
    return variantPayloadSizeImpl(variant, active);
}

uint64_t enumPayloadSize(const TypePtr& type) {
    if (!type || type->kind != TypeKind::Enum) return 0;
    uint64_t payload = 0;
    for (const auto& variant : type->variants)
        payload = std::max(payload, variantPayloadSize(variant));
    return std::max<uint64_t>(
        InlinePayloadAlignment,
        alignTo(payload, InlinePayloadAlignment));
}

std::string inlineAdtLayoutSignature(const TypePtr& type) {
    if (!type || (type->kind != TypeKind::Enum &&
                  type->kind != TypeKind::Result))
        return {};
    std::ostringstream output;
    output << "luna.inline-adt.v" << InlineAdtAbiVersion
           << ";tag_storage=" << InlineTagStorageSize
           << ";payload_align=" << InlinePayloadAlignment
           << ";size=" << valueSize(type);
    if (type->kind == TypeKind::Enum) {
        for (size_t variantIndex = 0;
             variantIndex < type->variants.size(); ++variantIndex) {
            const auto& variant = type->variants[variantIndex];
            output << ";variant=" << variantIndex << ':' << variant.name;
            for (size_t fieldIndex = 0;
                 fieldIndex < variant.fields.size(); ++fieldIndex)
                output << '@' << variantFieldOffset(variant, fieldIndex)
                       << '+' << valueSize(variant.fields[fieldIndex]);
        }
    }
    return output.str();
}

} // namespace luna::layout
