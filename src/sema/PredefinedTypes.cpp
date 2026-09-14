#include "PredefinedTypes.h"

#include "../parser/AST.h"

const std::vector<PredefinedTypeDefinition>& predefinedTypes() {
    static const std::vector<PredefinedTypeDefinition> definitions = {
        {"i8", PredefinedTypeForm::Atomic, 0, 0, false, TyI8},
        {"i16", PredefinedTypeForm::Atomic, 0, 0, false, TyI16},
        {"i32", PredefinedTypeForm::Atomic, 0, 0, false, TyI32},
        {"i64", PredefinedTypeForm::Atomic, 0, 0, false, TyI64},
        {"u8", PredefinedTypeForm::Atomic, 0, 0, false, TyU8},
        {"u16", PredefinedTypeForm::Atomic, 0, 0, false, TyU16},
        {"u32", PredefinedTypeForm::Atomic, 0, 0, false, TyU32},
        {"u64", PredefinedTypeForm::Atomic, 0, 0, false, TyU64},
        {"usize", PredefinedTypeForm::Atomic, 0, 0, false, TyUSize},
        {"isize", PredefinedTypeForm::Atomic, 0, 0, false, TyISize},
        {"f32", PredefinedTypeForm::Atomic, 0, 0, false, TyF32},
        {"f64", PredefinedTypeForm::Atomic, 0, 0, false, TyF64},
        {"bool", PredefinedTypeForm::Atomic, 0, 0, false, TyBool},
        {"string", PredefinedTypeForm::Atomic, 0, 0, false, TyString},
        {"cstr", PredefinedTypeForm::Atomic, 0, 0, false, TyCStr},
        {"unit", PredefinedTypeForm::Atomic, 0, 0, false, TyUnit},
        {"never", PredefinedTypeForm::Atomic, 0, 0, false, TyNever},
        {"event", PredefinedTypeForm::Atomic, 0, 0, false, TyEvent},
        {"raw", PredefinedTypeForm::RawPointer, 1, 1, false, nullptr},
        {"Result", PredefinedTypeForm::Result, 2, 2, false, nullptr},
        {"device_buffer", PredefinedTypeForm::DeviceBuffer, 1, 1, false, nullptr},
        {"array", PredefinedTypeForm::Array, 1, 1, true, nullptr},
        {"slice", PredefinedTypeForm::Slice, 1, 1, false, nullptr},
        {"metadata_view", PredefinedTypeForm::MetadataView, 1, 1, false, nullptr},
        {"symbol_set", PredefinedTypeForm::SymbolSet, 1, 1, false, nullptr},
        {"declaration_view", PredefinedTypeForm::DeclarationView, 0, 1, false, nullptr},
        {"declaration_ref", PredefinedTypeForm::DeclarationRef, 0, 1, false, nullptr},
    };
    return definitions;
}

const PredefinedTypeDefinition* findPredefinedType(std::string_view name) {
    for (const auto& definition : predefinedTypes()) {
        if (definition.name == name) return &definition;
    }
    return nullptr;
}

TypePtr findPredefinedAtomicType(std::string_view name) {
    const auto* definition = findPredefinedType(name);
    return definition && definition->form == PredefinedTypeForm::Atomic ? definition->atomicType
                                                                        : nullptr;
}

bool isPredefinedTypeName(std::string_view name) { return findPredefinedType(name) != nullptr; }

namespace {

std::string invalidArgumentsMessage(PredefinedTypeForm form) {
    switch (form) {
    case PredefinedTypeForm::Atomic:
        return "predefined atomic types do not accept type arguments";
    case PredefinedTypeForm::RawPointer:
        return "raw<T> requires exactly one type argument";
    case PredefinedTypeForm::Result:
        return "Result<T, E> requires value and error type arguments";
    case PredefinedTypeForm::DeviceBuffer:
        return "device_buffer<T> requires exactly one element type";
    case PredefinedTypeForm::Array:
        return "array<T, N> requires one element type and a non-negative compile-time length";
    case PredefinedTypeForm::Slice:
        return "slice<T> requires exactly one element type";
    case PredefinedTypeForm::MetadataView:
        return "metadata_view<M> requires exactly one metadata type";
    case PredefinedTypeForm::SymbolSet:
        return "symbol_set requires exactly one declaration type argument";
    case PredefinedTypeForm::DeclarationView:
        return "declaration_view accepts at most one callable type argument";
    case PredefinedTypeForm::DeclarationRef:
        return "declaration_ref accepts at most one callable type argument";
    }
    return "invalid predefined type arguments";
}

} // namespace

PredefinedTypeResolution
resolvePredefinedType(const NamedTypeAST& named,
                      const PredefinedTypeArgumentResolver& resolveArgument) {
    PredefinedTypeResolution result;
    const auto* definition = findPredefinedType(named.name);
    if (!definition) return result;

    result.recognized = true;
    result.form = definition->form;
    if (named.typeArgs.size() < definition->minimumTypeArguments ||
        named.typeArgs.size() > definition->maximumTypeArguments ||
        (definition->requiresArrayLength && !named.arrayLength)) {
        result.error = invalidArgumentsMessage(definition->form);
        return result;
    }

    if (definition->form == PredefinedTypeForm::Atomic) {
        result.type = definition->atomicType;
        return result;
    }

    TypePtr first =
        named.typeArgs.empty() ? nullptr : resolveArgument(named.typeArgs.front().get());
    switch (definition->form) {
    case PredefinedTypeForm::RawPointer:
        result.type = Type::makeRawPointer(first);
        break;
    case PredefinedTypeForm::Result:
        result.type = Type::makeResult(first, resolveArgument(named.typeArgs[1].get()));
        break;
    case PredefinedTypeForm::DeviceBuffer:
        result.type = Type::makeDeviceBuffer(first);
        break;
    case PredefinedTypeForm::Array:
        result.type = Type::makeArray(first, *named.arrayLength);
        break;
    case PredefinedTypeForm::Slice:
        result.type = Type::makeSlice(first);
        break;
    case PredefinedTypeForm::MetadataView:
        result.type = Type::makeMetadataView(first);
        break;
    case PredefinedTypeForm::SymbolSet:
        result.type = Type::makeSymbolSet(first);
        break;
    case PredefinedTypeForm::DeclarationView:
        result.type = Type::makeDeclarationView(first);
        break;
    case PredefinedTypeForm::DeclarationRef:
        result.type = Type::makeDeclarationRef(first);
        break;
    case PredefinedTypeForm::Atomic:
        break;
    }
    return result;
}
