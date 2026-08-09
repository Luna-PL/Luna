#pragma once

#include "StableIdentity.h"

#include <cstdint>
#include <memory>

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

using TypeId = luna::identity::TypeId;
using ShapeId = luna::identity::ShapeId;

} // namespace luna::types
