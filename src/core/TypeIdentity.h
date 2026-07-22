#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct Type;
using TypePtr = std::shared_ptr<Type>;

namespace luna::types {

enum class TypeDomain : uint8_t {
    Value,
    Meta,
    Compiler,
    Inference,
    Error,
};

enum class IdentityMode : uint8_t {
    Structural,
    Nominal,
    Builtin,
    MetaSchema,
    CompilerIntrinsic,
    Inference,
    Error,
};

struct TypeId {
    std::string value;

    bool empty() const { return value.empty(); }
    bool operator==(const TypeId& other) const { return value == other.value; }
    bool operator!=(const TypeId& other) const { return !(*this == other); }
};

struct ShapeId {
    std::string value;

    bool empty() const { return value.empty(); }
    bool operator==(const ShapeId& other) const { return value == other.value; }
    bool operator!=(const ShapeId& other) const { return !(*this == other); }
};

} // namespace luna::types
