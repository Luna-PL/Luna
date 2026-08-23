#pragma once

#include "Container.h"

#include <string>
#include <optional>
#include <vector>

namespace moon {

enum class ContainerPackageKind : uint32_t {
    Application = 1,
    Library = 2,
};

// Frozen 0.3 code-section tags. Zero is deliberately invalid so a cleared or
// truncated tag can never acquire executable meaning.
enum class CodeOperationOpcode : uint32_t {
    Let = 1, Allocate = 2, Expression = 3, Free = 4, Await = 5,
};

enum class CodeExpressionOpcode : uint32_t {
    Integer = 1, Floating = 2, String = 3, Boolean = 4, Unit = 5,
    Identifier = 6, Binary = 7, Unary = 8, Call = 9,
    DynamicSelect = 10, Launch = 11, VariantConstruct = 12,
    ResultConstruct = 13, FieldAccess = 14, Index = 15,
    SliceLength = 16, ArrayLiteral = 17, RecordLiteral = 18,
    HeapAllocate = 19, InitializeAllocation = 20, Move = 21,
    Borrow = 22, Dereference = 23, AddressOf = 24, Lambda = 25,
    MakeClosure = 26, EnvironmentLoad = 27, Assign = 28,
};

std::optional<CodeOperationOpcode> codeOperationOpcode(const Stmt& operation);
std::optional<CodeExpressionOpcode> codeExpressionOpcode(const Expr& expression);

// Target-specific package facts carried by the manifest section. Source paths
// and compiler-local filesystem state are intentionally not serialized.
struct ContainerManifest {
    std::string packageId;
    std::string packageVersion;
    ContainerPackageKind packageKind = ContainerPackageKind::Application;
    std::string targetTriple;
    std::string dataLayout;
    DeclarationRef entrypoint;
    FeatureFlags features;
};

class ContainerModelCodec {
public:
    static bool encodeManifest(
        const ContainerManifest& manifest,
        std::vector<uint8_t>& output,
        std::string& error,
        const ContainerLimits& limits = {});

    static bool decodeManifest(
        const std::vector<uint8_t>& input,
        ContainerManifest& manifest,
        std::string& error,
        const ContainerLimits& limits = {});

    static bool encodeTypes(
        const Module& module,
        std::vector<uint8_t>& output,
        std::string& error,
        const ContainerLimits& limits = {});

    // Decodes into a fresh module type snapshot. Existing non-type module
    // fields are retained, but the type table and its indexes are replaced.
    static bool decodeTypes(
        const std::vector<uint8_t>& input,
        Module& module,
        std::string& error,
        const ContainerLimits& limits = {});

    static bool encodeSymbols(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});
    static bool encodeContracts(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});
    static bool encodeSysmeta(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});

    // Atomically joins the three normalized sections by SymbolId. No partial
    // declaration table or metadata schema state is published on failure.
    static bool decodeDeclarations(
        const std::vector<uint8_t>& symbols,
        const std::vector<uint8_t>& contracts,
        const std::vector<uint8_t>& sysmeta,
        Module& module,
        std::string& error,
        const ContainerLimits& limits = {});

    static bool encodeImports(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});
    static bool encodeExports(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});
    static bool decodeInterfaces(
        const std::vector<uint8_t>& imports,
        const std::vector<uint8_t>& exports,
        Module& module, std::string& error,
        const ContainerLimits& limits = {});

    static bool encodeCode(
        const Module& module, std::vector<uint8_t>& output,
        std::string& error, const ContainerLimits& limits = {});
    // Atomically replaces executable declarations after all function rows
    // have joined the already-decoded declaration table successfully.
    static bool decodeCode(
        const std::vector<uint8_t>& input, Module& module,
        std::string& error, const ContainerLimits& limits = {});

    // Encodes or decodes all eight required sections as one authenticated
    // container. Decode publishes neither manifest nor module until the
    // complete model has passed the MoonIR verifier.
    static bool encodeContainer(
        const ContainerManifest& manifest, const Module& module,
        std::vector<uint8_t>& output, std::string& error,
        const ContainerLimits& limits = {});
    static bool decodeContainer(
        const std::vector<uint8_t>& input, ContainerManifest& manifest,
        Module& module, std::string& error,
        const ContainerLimits& limits = {});
    // Executable loading additionally binds the host-specific manifest facts.
    // A mismatch publishes neither manifest nor module.
    static bool decodeContainerForTarget(
        const std::vector<uint8_t>& input,
        const std::string& expectedTargetTriple,
        const std::string& expectedDataLayout,
        ContainerManifest& manifest, Module& module,
        std::string& error, const ContainerLimits& limits = {});
};

} // namespace moon
