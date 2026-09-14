#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "parser/AST.h"
#include "runtime/RuntimeABI.h"
#include "sema/PredefinedTypes.h"
#include "sema/SymbolTable.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

struct AtomicExpectation {
    const char* name;
    TypePtr type;
    TypeKind kind;
    uint64_t size;
    uint64_t alignment;
};

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::unique_ptr<NamedTypeAST> namedType(const char* name) {
    return std::make_unique<NamedTypeAST>(name);
}

} // namespace

int main() {
    static_assert(offsetof(LunaDeviceBufferI32V1, data) == 0);
    static_assert(offsetof(LunaDeviceBufferI32V1, length) == sizeof(void*));

    const std::array<AtomicExpectation, 18> atomicTypes = {{
        {"i8", TyI8, TypeKind::I8, 1, 1},
        {"i16", TyI16, TypeKind::I16, 2, 2},
        {"i32", TyI32, TypeKind::I32, 4, 4},
        {"i64", TyI64, TypeKind::I64, 8, 8},
        {"u8", TyU8, TypeKind::U8, 1, 1},
        {"u16", TyU16, TypeKind::U16, 2, 2},
        {"u32", TyU32, TypeKind::U32, 4, 4},
        {"u64", TyU64, TypeKind::U64, 8, 8},
        {"usize", TyUSize, TypeKind::USize, 8, 8},
        {"isize", TyISize, TypeKind::ISize, 8, 8},
        {"f32", TyF32, TypeKind::F32, 4, 4},
        {"f64", TyF64, TypeKind::F64, 8, 8},
        {"bool", TyBool, TypeKind::Bool, 1, 1},
        {"string", TyString, TypeKind::String, 8, 8},
        {"cstr", TyCStr, TypeKind::CStr, 8, 8},
        {"unit", TyUnit, TypeKind::Unit, 0, 1},
        {"never", TyNever, TypeKind::Never, 0, 1},
        {"event", TyEvent, TypeKind::Event, 4, 4},
    }};

    if (!expect(sizeof(void*) == 8 && sizeof(size_t) == 8,
                "the frozen builtin layout test requires a 64-bit target") ||
        !expect(sizeof(LunaDeviceBufferI32V1) == 16,
                "device buffer C carrier is not two machine words") ||
        !expect(predefinedTypes().size() == 27,
                "predefined type registry inventory changed unexpectedly"))
        return 1;

    SymbolTable symbols;
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> typeIds;
    for (const auto& expectation : atomicTypes) {
        const auto registered = findPredefinedAtomicType(expectation.name);
        const std::string canonical = "5:value;" +
                                      std::to_string(std::string(expectation.name).size()) + ":" +
                                      expectation.name + ";";
        if (!expect(registered == expectation.type,
                    "predefined atomic singleton identity changed") ||
            !expect(registered->kind == expectation.kind &&
                        registered->identityMode == luna::types::IdentityMode::Builtin,
                    "predefined atomic kind or identity mode changed") ||
            !expect(luna::layout::valueSize(registered) == expectation.size &&
                        luna::layout::valueAlignment(registered) == expectation.alignment,
                    "predefined atomic value layout changed") ||
            !expect(luna::types::canonicalType(registered) == canonical,
                    "predefined atomic canonical identity changed") ||
            !expect(luna::types::typeId(registered) == luna::types::typeIdFromCanonical(canonical),
                    "predefined atomic TypeId changed") ||
            !expect(symbols.lookupType(expectation.name) == registered,
                    "root type namespace omitted a predefined atomic type"))
            return 2;
        names.insert(expectation.name);
        typeIds.insert(luna::types::typeId(registered).value);
    }
    if (!expect(names.size() == atomicTypes.size(), "predefined atomic names are not unique") ||
        !expect(typeIds.size() == atomicTypes.size(), "predefined atomic TypeIds are not unique") ||
        !expect(!symbols.defineType("i32", Type::makeStruct("i32")) &&
                    !symbols.defineType("raw", Type::makeStruct("raw")),
                "predefined type binding was replaceable"))
        return 3;

    NamedTypeAST raw("raw");
    raw.typeArgs.push_back(namedType("i32"));
    NamedTypeAST result("Result");
    result.typeArgs.push_back(namedType("i32"));
    result.typeArgs.push_back(namedType("bool"));
    NamedTypeAST deviceBuffer("device_buffer");
    deviceBuffer.typeArgs.push_back(namedType("i32"));
    NamedTypeAST array("array");
    array.typeArgs.push_back(namedType("i16"));
    array.arrayLength = 3;
    NamedTypeAST slice("slice");
    slice.typeArgs.push_back(namedType("i32"));

    const std::unordered_map<std::string, TypePtr> forgedBinding = {
        {"i32", Type::makeStruct("forged-i32")},
    };
    const auto resolvedRaw = resolveType(&raw, forgedBinding);
    const auto resolvedResult = resolveType(&result, forgedBinding);
    const auto resolvedDeviceBuffer = resolveType(&deviceBuffer, forgedBinding);
    const auto resolvedArray = resolveType(&array, forgedBinding);
    const auto resolvedSlice = resolveType(&slice, forgedBinding);
    if (!expect(resolvedRaw->kind == TypeKind::RawPointer && resolvedRaw->inner == TyI32,
                "raw<T> formation or predefined precedence changed") ||
        !expect(resolvedResult->kind == TypeKind::Result && resolvedResult->typeArgs.size() == 2 &&
                    resolvedResult->typeArgs[0] == TyI32 && resolvedResult->typeArgs[1] == TyBool,
                "Result<T, E> formation changed") ||
        !expect(resolvedDeviceBuffer->kind == TypeKind::DeviceBuffer &&
                    resolvedDeviceBuffer->inner == TyI32 &&
                    luna::layout::valueSize(resolvedDeviceBuffer) ==
                        sizeof(LunaDeviceBufferI32V1) &&
                    luna::layout::valueAlignment(resolvedDeviceBuffer) == 8,
                "device_buffer<T> formation or C ABI layout changed") ||
        !expect(resolvedArray->kind == TypeKind::Array && resolvedArray->inner == TyI16 &&
                    luna::layout::valueSize(resolvedArray) == 6 &&
                    luna::layout::valueAlignment(resolvedArray) == 2,
                "array<T, N> formation or inline layout changed") ||
        !expect(resolvedSlice->kind == TypeKind::Slice && resolvedSlice->inner == TyI32 &&
                    luna::layout::valueSize(resolvedSlice) == 16 &&
                    luna::layout::valueAlignment(resolvedSlice) == 8,
                "slice<T> formation or value layout changed"))
        return 4;

    NamedTypeAST invalidAtomic("i32");
    invalidAtomic.typeArgs.push_back(namedType("bool"));
    NamedTypeAST invalidArray("array");
    invalidArray.typeArgs.push_back(namedType("i32"));
    if (!expect(resolveType(&invalidAtomic, {}).get() == TyUnknown.get(),
                "atomic type accepted a type argument") ||
        !expect(resolveType(&invalidArray, {}).get() == TyUnknown.get(),
                "array type accepted a missing extent"))
        return 5;

    auto product = Type::makeStruct("Aligned", {{"small", TyI8}, {"wide", TyI64}}, "test::Aligned");
    if (!expect(luna::layout::valueSize(product) == 8,
                "nominal product value stopped being pointer represented") ||
        !expect(luna::layout::productFieldOffset(product, 0) == 0 &&
                    luna::layout::productFieldOffset(product, 1) == 8 &&
                    luna::layout::productStorageSize(product) == 16 &&
                    luna::layout::productStorageAlignment(product) == 8,
                "nominal product storage layout changed"))
        return 6;

    return 0;
}
