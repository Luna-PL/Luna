#pragma once

#include "TypeSystem.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct NamedTypeAST;
struct TypeAST;

enum class PredefinedTypeForm {
    Atomic,
    RawPointer,
    Result,
    DeviceBuffer,
    Array,
    Slice,
    MetadataView,
    SymbolSet,
    DeclarationView,
    DeclarationRef,
};

struct PredefinedTypeDefinition {
    std::string_view name;
    PredefinedTypeForm form = PredefinedTypeForm::Atomic;
    size_t minimumTypeArguments = 0;
    size_t maximumTypeArguments = 0;
    bool requiresArrayLength = false;
    TypePtr atomicType;
};

struct PredefinedTypeResolution {
    bool recognized = false;
    PredefinedTypeForm form = PredefinedTypeForm::Atomic;
    TypePtr type;
    std::string error;
};

using PredefinedTypeArgumentResolver = std::function<TypePtr(const TypeAST*)>;

const std::vector<PredefinedTypeDefinition>& predefinedTypes();
const PredefinedTypeDefinition* findPredefinedType(std::string_view name);
TypePtr findPredefinedAtomicType(std::string_view name);
bool isPredefinedTypeName(std::string_view name);

PredefinedTypeResolution
resolvePredefinedType(const NamedTypeAST& named,
                      const PredefinedTypeArgumentResolver& resolveArgument);
