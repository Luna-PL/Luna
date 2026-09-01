#include "moonir/ContainerModel.h"
#include "moonir/Verifier.h"
#include "core/TypeRelations.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

static_assert(static_cast<uint32_t>(moon::CodeOperationOpcode::Let) == 1);
static_assert(static_cast<uint32_t>(moon::CodeOperationOpcode::Await) == 5);
static_assert(static_cast<uint32_t>(moon::CodeExpressionOpcode::Integer) == 1);
static_assert(static_cast<uint32_t>(
                  moon::CodeExpressionOpcode::ReservedDynamicSelect) == 10);
static_assert(static_cast<uint32_t>(moon::CodeExpressionOpcode::Assign) == 28);

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] =
            static_cast<uint8_t>(value >> (index * 8));
}

std::optional<size_t> firstOperationOpcodeOffset(
    const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    const auto takeU32 = [&]() -> std::optional<uint32_t> {
        if (offset > bytes.size() || bytes.size() - offset < 4)
            return std::nullopt;
        uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index)
            value |= static_cast<uint32_t>(bytes[offset + index]) <<
                     (index * 8);
        offset += 4;
        return value;
    };
    const auto skipString = [&]() -> bool {
        const auto size = takeU32();
        if (!size || offset > bytes.size() ||
            *size > bytes.size() - offset) return false;
        offset += *size;
        return true;
    };
    const auto skipLocation = [&]() -> bool {
        if (!skipString() || offset > bytes.size() ||
            bytes.size() - offset < 16) return false;
        offset += 16;
        return true;
    };

    const auto functions = takeU32();
    if (!functions || *functions != 1) return std::nullopt;
    for (unsigned index = 0; index < 6; ++index)
        if (!skipString()) return std::nullopt;
    if (offset > bytes.size() || bytes.size() - offset < 20)
        return std::nullopt;
    offset += 20;
    for (unsigned index = 0; index < 2; ++index)
        if (!skipString()) return std::nullopt;
    const auto typeParameters = takeU32();
    if (!typeParameters) return std::nullopt;
    for (uint32_t index = 0; index < *typeParameters; ++index)
        if (!skipString()) return std::nullopt;
    const auto parameters = takeU32();
    if (!parameters || *parameters != 0 || !skipString())
        return std::nullopt;
    if (offset > bytes.size() || bytes.size() - offset < 12)
        return std::nullopt;
    offset += 12;
    const auto arguments = takeU32();
    if (!arguments) return std::nullopt;
    for (uint32_t index = 0; index < *arguments; ++index)
        if (!skipString()) return std::nullopt;
    if (!skipLocation()) return std::nullopt;
    const auto hasGraph = takeU32();
    if (!hasGraph || *hasGraph != 1 || offset > bytes.size() ||
        bytes.size() - offset < 16) return std::nullopt;
    offset += 16;
    const auto blocks = takeU32();
    if (!blocks || *blocks == 0 || offset > bytes.size() ||
        bytes.size() - offset < 12) return std::nullopt;
    offset += 12;
    if (!skipLocation()) return std::nullopt;
    const auto operations = takeU32();
    if (!operations || *operations == 0 ||
        offset > bytes.size() || bytes.size() - offset < 4)
        return std::nullopt;
    return offset;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string error;

    moon::LetStmt canonicalLet;
    moon::ReturnStmt structuredReturn;
    moon::IntLiteralExpr canonicalInteger;
    moon::TryExpr structuredTry;
    if (moon::codeOperationOpcode(canonicalLet) !=
            moon::CodeOperationOpcode::Let ||
        moon::codeOperationOpcode(structuredReturn).has_value() ||
        moon::codeExpressionOpcode(canonicalInteger) !=
            moon::CodeExpressionOpcode::Integer ||
        moon::codeExpressionOpcode(structuredTry).has_value())
        return fail("code opcode classifier accepts a structured-only node");

    moon::ContainerManifest manifest;
    manifest.packageId = "example.codec";
    manifest.packageVersion = "0.3.0";
    manifest.packageKind = moon::ContainerPackageKind::Application;
    manifest.targetTriple = "x86_64-unknown-linux-gnu";
    manifest.dataLayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64";
    manifest.entrypoint.symbol = luna::identity::SymbolId{"symbol:main"};
    manifest.entrypoint.contract =
        luna::identity::ContractId{"contract:main"};
    manifest.features.runtime = true;

    std::vector<uint8_t> manifestBytes;
    if (!moon::ContainerModelCodec::encodeManifest(
            manifest, manifestBytes, error))
        return fail(error);
    moon::ContainerManifest decodedManifest;
    if (!moon::ContainerModelCodec::decodeManifest(
            manifestBytes, decodedManifest, error))
        return fail(error);
    std::vector<uint8_t> canonicalManifestBytes;
    if (!moon::ContainerModelCodec::encodeManifest(
            decodedManifest, canonicalManifestBytes, error) ||
        canonicalManifestBytes != manifestBytes)
        return fail("manifest model codec is not canonical");
    if (decodedManifest.packageId != manifest.packageId ||
        decodedManifest.packageVersion != manifest.packageVersion ||
        decodedManifest.packageKind != manifest.packageKind ||
        decodedManifest.targetTriple != manifest.targetTriple ||
        decodedManifest.dataLayout != manifest.dataLayout ||
        decodedManifest.entrypoint != manifest.entrypoint ||
        !decodedManifest.features.runtime ||
        decodedManifest.features.kernelRuntimeReserved)
        return fail("manifest model codec lost semantic fields");

    auto malformedManifest = manifestBytes;
    writeU32(malformedManifest, malformedManifest.size() - 4, 1u << 31);
    if (moon::ContainerModelCodec::decodeManifest(
            malformedManifest, decodedManifest, error))
        return fail("manifest decoder accepted unknown feature flags");
    malformedManifest = manifestBytes;
    writeU32(malformedManifest, malformedManifest.size() - 4, 1u << 1);
    if (moon::ContainerModelCodec::decodeManifest(
            malformedManifest, decodedManifest, error))
        return fail("manifest decoder accepted retired Dynamic retention");
    malformedManifest = manifestBytes;
    writeU32(malformedManifest, malformedManifest.size() - 4, 1u << 2);
    if (moon::ContainerModelCodec::decodeManifest(
            malformedManifest, decodedManifest, error))
        return fail("manifest decoder accepted the retired dynamic-apply feature");
    malformedManifest = manifestBytes;
    writeU32(malformedManifest, malformedManifest.size() - 4, 1u << 3);
    if (moon::ContainerModelCodec::decodeManifest(
            malformedManifest, decodedManifest, error))
        return fail("manifest decoder accepted the retired dynamic-select feature");
    malformedManifest = manifestBytes;
    malformedManifest.push_back(0);
    if (moon::ContainerModelCodec::decodeManifest(
            malformedManifest, decodedManifest, error))
        return fail("manifest decoder accepted trailing bytes");

    moon::ContainerManifest invalidUtf8 = manifest;
    invalidUtf8.packageId.assign(1, static_cast<char>(0xff));
    if (moon::ContainerModelCodec::encodeManifest(
            invalidUtf8, canonicalManifestBytes, error) ||
        error.find("UTF-8") == std::string::npos)
        return fail("manifest encoder accepted invalid UTF-8");

    auto i32 = Type::makePrimitive(TypeKind::I32);
    auto boolean = Type::makePrimitive(TypeKind::Bool);
    auto string = Type::makePrimitive(TypeKind::String);
    auto pair = Type::makeRecord({{"left", i32}, {"right", string}});
    auto array = Type::makeArray(pair, 3);
    auto result = Type::makeResult(array, boolean);
    auto closure = Type::makeClosure(
        {i32, Type::makeReference(string)}, result,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy},
         {luna::ownership::Relation::SharedBorrow,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine},
        {{"captured", string}});
    closure->sysmeta.capability.runtimeRetained = true;
    closure->sysmeta.abi.stableBoundary = true;

    moon::Module source;
    source.name = "example.codec";
    source.features.runtime = true;
    const auto root = source.registerType(closure);
    const auto f64 = source.registerType(Type::makePrimitive(TypeKind::F64));
    const auto i32Ref = source.registerType(i32);
    const auto handlerFunctionType = Type::makeFunction({}, i32);
    const auto handlerType = source.registerType(handlerFunctionType);

    moon::MetadataSchema schema;
    schema.id = "example.codec::meta::CodecFacts";
    schema.name = "CodecFacts";
    schema.fields = {{"count", i32Ref},
                     {"ratio", f64},
                     {"enabled", source.registerType(boolean)},
                     {"label", source.registerType(string)}};
    schema.location = {"codec.luna", 1, 1};
    source.metadataSchemas.push_back(schema);

    moon::DeclarationRecord declaration;
    declaration.id = "example.codec::fn::handler";
    declaration.familyId = declaration.id;
    declaration.symbolId = luna::identity::symbolIdFromCanonical(declaration.id);
    declaration.sourceName = "handler";
    declaration.linkageName = "example_codec_handler";
    declaration.kind = moon::DeclarationKind::Function;
    declaration.retention = moon::Retention::Runtime;
    declaration.type = handlerType;
    declaration.sysmeta = handlerFunctionType->sysmeta;
    declaration.sysmeta.capability.runtimeRetained = true;
    declaration.location = {"codec.luna", 5, 3};
    moon::MetadataInstance metadata;
    metadata.schemaId = schema.id;
    metadata.values = {int64_t{7}, 0.5, true, std::string{"codec"}};
    metadata.retention = moon::Retention::Runtime;
    metadata.location = declaration.location;
    declaration.metadata.push_back(std::move(metadata));
    source.declarationTable.push_back(std::move(declaration));
    source.sealTypeTable();
    const auto* handler = source.findDeclarationById(
        "example.codec::fn::handler");
    if (!handler) return fail("sealed source lost handler declaration");

    auto executable = std::make_unique<moon::FunctionDecl>();
    executable->packageId = source.name;
    executable->modulePath = "codec";
    executable->declarationId = handler->id;
    executable->familyId = handler->familyId;
    executable->symbolId = handler->symbolId;
    executable->contractId = handler->contractId;
    executable->name = handler->sourceName;
    executable->generatedSymbolName = handler->linkageName;
    executable->isExported = true;
    executable->retention = handler->retention;
    executable->metadata = handler->metadata;
    executable->sysmeta = handler->sysmeta;
    executable->returnType = i32Ref;
    executable->location = handler->location;
    executable->controlFlow = std::make_unique<moon::ControlFlowGraph>();
    auto& graph = *executable->controlFlow;
    graph.sealed = true;
    graph.entry = moon::BlockId{0};
    graph.rootRegion = moon::RegionId{0};
    graph.rootScope = moon::ScopeId{0};
    moon::BasicBlock block;
    block.id = moon::BlockId{0};
    block.region = moon::RegionId{0};
    block.scope = moon::ScopeId{0};
    block.location = handler->location;
    auto expressionStatement = std::make_unique<moon::ExprStmt>();
    auto sideEffectValue = std::make_unique<moon::IntLiteralExpr>();
    sideEffectValue->type = i32Ref;
    sideEffectValue->value = 7;
    expressionStatement->expr = std::move(sideEffectValue);
    block.operations.push_back(std::move(expressionStatement));
    block.terminator.kind = moon::TerminatorKind::Return;
    block.terminator.location = handler->location;
    auto returnValue = std::make_unique<moon::IntLiteralExpr>();
    returnValue->type = i32Ref;
    returnValue->value = 42;
    block.terminator.operand = std::move(returnValue);
    graph.blocks.push_back(std::move(block));
    moon::RegionRecord region;
    region.id = moon::RegionId{0};
    region.kind = moon::RegionKind::Function;
    region.scope = moon::ScopeId{0};
    region.entry = moon::BlockId{0};
    region.blocks = {moon::BlockId{0}};
    region.location = handler->location;
    graph.regions.push_back(std::move(region));
    moon::ScopeRecord scope;
    scope.id = moon::ScopeId{0};
    scope.region = moon::RegionId{0};
    scope.location = handler->location;
    graph.scopes.push_back(std::move(scope));
    source.declarations.push_back(std::move(executable));
    source.rebuildIndexes();
    source.packageUses.push_back({"example.codec", "dependency.codec", "dep"});
    moon::ImportRecord packageImport;
    packageImport.kind = moon::ImportKind::Package;
    packageImport.ownerPackageId = "example.codec";
    packageImport.packageId = "dependency.codec";
    packageImport.alias = "dep";
    source.imports.push_back(packageImport);
    moon::ImportRecord hostImport;
    hostImport.kind = moon::ImportKind::Host;
    hostImport.ownerPackageId = "example.codec";
    hostImport.localName = "io::write";
    hostImport.capabilityId = "org.luna.host.console.write";
    hostImport.linkSymbol = "example_codec_handler";
    hostImport.abi = "C";
    hostImport.declaration = {handler->symbolId, handler->contractId};
    hostImport.type = handler->type;
    source.imports.push_back(hostImport);
    moon::ExportRecord exported;
    exported.name = "handler";
    exported.declaration = {handler->symbolId, handler->contractId};
    exported.type = handler->type;
    exported.kind = handler->kind;
    exported.abi = "C";
    exported.location = handler->location;
    source.exports.push_back(exported);

    std::vector<uint8_t> typeBytes;
    if (!moon::ContainerModelCodec::encodeTypes(source, typeBytes, error))
        return fail(error);

    moon::Module decoded;
    decoded.name = source.name;
    decoded.features = decodedManifest.features;
    if (!moon::ContainerModelCodec::decodeTypes(typeBytes, decoded, error))
        return fail(error);
    if (!decoded.typeTableSealed ||
        decoded.typeTable.size() != source.typeTable.size())
        return fail("type decoder did not construct a sealed independent table");

    std::vector<uint8_t> canonicalTypeBytes;
    if (!moon::ContainerModelCodec::encodeTypes(
            decoded, canonicalTypeBytes, error) ||
        canonicalTypeBytes != typeBytes)
        return fail("type model codec is not byte-canonical after decode");

    moon::TypeMaterializer materializer(decoded);
    for (const auto& record : decoded.typeTable) {
        const auto restored = materializer.materialize(record.id);
        if (!restored || luna::types::typeId(restored) != record.id ||
            luna::types::canonicalType(restored) != record.canonicalType)
            return fail("independent TypeMaterializer changed a decoded TypeId");
    }
    const auto restoredRoot = materializer.materialize(root);
    if (!restoredRoot || restoredRoot->kind != TypeKind::Closure ||
        restoredRoot->capturedFields.size() != 1 ||
        !restoredRoot->sysmeta.capability.runtimeRetained ||
        !restoredRoot->sysmeta.abi.stableBoundary)
        return fail("decoded type graph lost closure or sysmeta payload");

    moon::Verifier verifier;
    if (!verifier.verify(decoded)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Verifier rejected the independently decoded type module");
    }

    std::vector<uint8_t> symbolBytes;
    std::vector<uint8_t> contractBytes;
    std::vector<uint8_t> sysmetaBytes;
    if (!moon::ContainerModelCodec::encodeSymbols(
            source, symbolBytes, error) ||
        !moon::ContainerModelCodec::encodeContracts(
            source, contractBytes, error) ||
        !moon::ContainerModelCodec::encodeSysmeta(
            source, sysmetaBytes, error))
        return fail(error);
    if (!moon::ContainerModelCodec::decodeDeclarations(
            symbolBytes, contractBytes, sysmetaBytes, decoded, error))
        return fail(error);
    if (decoded.declarationTable.size() != 1 ||
        decoded.metadataSchemas.size() != 1 ||
        decoded.declarationTable[0].metadata.size() != 1 ||
        decoded.declarationTable[0].metadata[0].values.size() != 4)
        return fail("normalized declaration sections lost metadata or rows");
    std::vector<uint8_t> canonicalSymbols;
    std::vector<uint8_t> canonicalContracts;
    std::vector<uint8_t> canonicalSysmeta;
    if (!moon::ContainerModelCodec::encodeSymbols(
            decoded, canonicalSymbols, error) ||
        !moon::ContainerModelCodec::encodeContracts(
            decoded, canonicalContracts, error) ||
        !moon::ContainerModelCodec::encodeSysmeta(
            decoded, canonicalSysmeta, error) ||
        canonicalSymbols != symbolBytes ||
        canonicalContracts != contractBytes ||
        canonicalSysmeta != sysmetaBytes)
        return fail("normalized declaration codec is not byte-canonical");
    if (!verifier.verify(decoded)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Verifier rejected independently joined declaration sections");
    }

    std::vector<uint8_t> importBytes;
    std::vector<uint8_t> exportBytes;
    if (!moon::ContainerModelCodec::encodeImports(
            source, importBytes, error) ||
        !moon::ContainerModelCodec::encodeExports(
            source, exportBytes, error) ||
        !moon::ContainerModelCodec::decodeInterfaces(
            importBytes, exportBytes, decoded, error))
        return fail(error);
    if (decoded.imports.size() != 2 || decoded.exports.size() != 1 ||
        decoded.packageUses.size() != 1 ||
        decoded.imports[1].capabilityId !=
            "org.luna.host.console.write" ||
        decoded.exports[0].declaration != hostImport.declaration)
        return fail("import/export codec lost canonical interface facts");
    if (!verifier.verify(decoded)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Verifier rejected independently decoded interfaces");
    }

    std::vector<uint8_t> codeBytes;
    if (!moon::ContainerModelCodec::encodeCode(
            source, codeBytes, error) ||
        !moon::ContainerModelCodec::decodeCode(
            codeBytes, decoded, error))
        return fail(error);
    if (decoded.declarations.size() != 1)
        return fail("code decoder lost its function row");
    const auto* decodedFunction = dynamic_cast<const moon::FunctionDecl*>(
        decoded.declarations[0].get());
    if (!decodedFunction || !decodedFunction->controlFlow ||
        decodedFunction->controlFlow->blocks.size() != 1 ||
        decodedFunction->controlFlow->blocks[0].operations.size() != 1 ||
        decodedFunction->returnType != i32Ref ||
        !decodedFunction->isExported)
        return fail("code decoder lost executable function or CFG facts");
    std::vector<uint8_t> canonicalCodeBytes;
    if (!moon::ContainerModelCodec::encodeCode(
            decoded, canonicalCodeBytes, error) ||
        canonicalCodeBytes != codeBytes)
        return fail("code model codec is not byte-canonical after decode");
    if (!verifier.verify(decoded)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Verifier rejected the independently decoded code section");
    }

    const auto operationOpcode = firstOperationOpcodeOffset(codeBytes);
    if (!operationOpcode)
        return fail("independent code cursor could not find the first opcode");
    auto unknownOpcode = codeBytes;
    writeU32(unknownOpcode, *operationOpcode, 0);
    const auto* preservedDeclaration = decoded.declarations[0].get();
    if (moon::ContainerModelCodec::decodeCode(
            unknownOpcode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted opcode zero or mutated output on failure");
    unknownOpcode = codeBytes;
    writeU32(unknownOpcode, *operationOpcode, 6);
    if (moon::ContainerModelCodec::decodeCode(
            unknownOpcode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted an unknown operation opcode");
    const size_t expressionOpcode = *operationOpcode + 28;
    unknownOpcode = codeBytes;
    writeU32(unknownOpcode, expressionOpcode, 0);
    if (moon::ContainerModelCodec::decodeCode(
            unknownOpcode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted expression opcode zero");
    unknownOpcode = codeBytes;
    writeU32(unknownOpcode, expressionOpcode, 10);
    if (moon::ContainerModelCodec::decodeCode(
            unknownOpcode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted retired dynamic-select opcode 10");
    unknownOpcode = codeBytes;
    writeU32(unknownOpcode, expressionOpcode, 29);
    if (moon::ContainerModelCodec::decodeCode(
            unknownOpcode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted an unknown expression opcode");
    auto truncatedCode = codeBytes;
    truncatedCode.pop_back();
    if (moon::ContainerModelCodec::decodeCode(
            truncatedCode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted truncation or partially mutated output");
    auto trailingCode = codeBytes;
    trailingCode.push_back(0);
    if (moon::ContainerModelCodec::decodeCode(
            trailingCode, decoded, error) ||
        decoded.declarations[0].get() != preservedDeclaration)
        return fail("code decoder accepted trailing bytes or mutated output");

    moon::ContainerManifest containerManifest = manifest;
    containerManifest.packageId = source.name;
    containerManifest.entrypoint = {
        handler->symbolId, handler->contractId};
    containerManifest.features = source.features;
    std::vector<uint8_t> containerBytes;
    if (!moon::ContainerModelCodec::encodeContainer(
            containerManifest, source, containerBytes, error))
        return fail(error);
    moon::ContainerManifest loadedManifest;
    moon::Module loadedModule;
    if (!moon::ContainerModelCodec::decodeContainer(
            containerBytes, loadedManifest, loadedModule, error))
        return fail(error);
    if (loadedManifest.entrypoint != containerManifest.entrypoint ||
        loadedModule.name != source.name ||
        loadedModule.declarations.size() != 1)
        return fail("whole-container decode lost manifest or executable state");
    moon::ContainerManifest mismatchedTargetManifest;
    mismatchedTargetManifest.packageId = "untouched.target";
    moon::Module mismatchedTargetModule;
    mismatchedTargetModule.name = "untouched.target.module";
    if (moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, "different-target", containerManifest.dataLayout,
            mismatchedTargetManifest, mismatchedTargetModule, error) ||
        mismatchedTargetManifest.packageId != "untouched.target" ||
        mismatchedTargetModule.name != "untouched.target.module")
        return fail("executable container loader accepted a target mismatch");
    std::vector<uint8_t> canonicalContainerBytes;
    if (!moon::ContainerModelCodec::encodeContainer(
            loadedManifest, loadedModule, canonicalContainerBytes, error) ||
        canonicalContainerBytes != containerBytes)
        return fail("whole-container codec is not byte-canonical");

    moon::ContainerReader parsedContainer;
    if (!parsedContainer.parse(containerBytes, error)) return fail(error);

    const auto checkMutation = [&](const std::vector<uint8_t>& bytes,
                                   bool authenticatedMayRemainValid)
        -> std::string {
        moon::ContainerManifest sentinelManifest;
        sentinelManifest.packageId = "mutation.sentinel.manifest";
        moon::Module sentinelModule;
        sentinelModule.name = "mutation.sentinel.module";
        const bool accepted = moon::ContainerModelCodec::decodeContainer(
            bytes, sentinelManifest, sentinelModule, error);
        if (!accepted) {
            if (sentinelManifest.packageId != "mutation.sentinel.manifest" ||
                sentinelModule.name != "mutation.sentinel.module" ||
                sentinelModule.typeTableSealed ||
                !sentinelModule.typeTable.empty() ||
                !sentinelModule.declarationTable.empty() ||
                !sentinelModule.declarations.empty())
                return "mutation failure published partial container state";
            return {};
        }
        if (!authenticatedMayRemainValid)
            return "unauthenticated byte mutation passed container integrity";
        std::vector<uint8_t> canonicalMutation;
        if (!moon::ContainerModelCodec::encodeContainer(
                sentinelManifest, sentinelModule,
                canonicalMutation, error))
            return "verified payload mutation could not be re-encoded: " + error;
        if (canonicalMutation != bytes)
            return "verified payload mutation was not byte-canonical";
        return {};
    };

    const size_t rawMutationCount = std::min<size_t>(
        512, containerBytes.size());
    for (size_t sample = 0; sample < rawMutationCount; ++sample) {
        auto corrupted = containerBytes;
        const size_t offset = sample * corrupted.size() / rawMutationCount;
        corrupted[offset] ^= static_cast<uint8_t>(1u << (sample % 8));
        const auto mutationError = checkMutation(corrupted, false);
        if (!mutationError.empty()) return fail(mutationError);
    }
    for (size_t sample = 0; sample < 32; ++sample) {
        auto truncated = containerBytes;
        truncated.resize(sample * containerBytes.size() / 32);
        const auto mutationError = checkMutation(truncated, false);
        if (!mutationError.empty()) return fail(mutationError);
    }

    size_t authenticatedMutationCount = 0;
    for (size_t sectionIndex = 0;
         sectionIndex < parsedContainer.sections().size(); ++sectionIndex) {
        const auto& payload = parsedContainer.sections()[sectionIndex].payload;
        const size_t samples = std::min<size_t>(24, payload.size());
        for (size_t sample = 0; sample < samples; ++sample) {
            auto mutatedSections = parsedContainer.sections();
            auto& mutatedPayload = mutatedSections[sectionIndex].payload;
            const size_t offset = sample * mutatedPayload.size() / samples;
            mutatedPayload[offset] ^=
                static_cast<uint8_t>(1u << (sample % 8));
            std::vector<uint8_t> authenticatedMutation;
            if (!moon::ContainerWriter::encode(
                    std::move(mutatedSections), authenticatedMutation, error))
                return fail("could not authenticate a payload mutation: " + error);
            const auto mutationError = checkMutation(
                authenticatedMutation, true);
            if (!mutationError.empty())
                return fail(
                    "authenticated mutation section " +
                    std::to_string(sectionIndex + 1) + ", sample " +
                    std::to_string(sample) + ", offset " +
                    std::to_string(offset) + ": " + mutationError);
            ++authenticatedMutationCount;
        }
    }
    if (authenticatedMutationCount < 100)
        return fail("payload mutation suite did not reach its minimum case count");

    auto forgedSections = parsedContainer.sections();
    auto* sourceGraph = dynamic_cast<moon::FunctionDecl*>(
        source.declarations[0].get())->controlFlow.get();
    sourceGraph->blocks[0].region = moon::RegionId{99};
    std::vector<uint8_t> invalidGraphCode;
    if (!moon::ContainerModelCodec::encodeCode(
            source, invalidGraphCode, error)) return fail(error);
    sourceGraph->blocks[0].region = moon::RegionId{0};
    for (auto& section : forgedSections)
        if (section.id == static_cast<uint32_t>(
                moon::ContainerSectionId::Code))
            section.payload = invalidGraphCode;
    std::vector<uint8_t> forgedContainer;
    if (!moon::ContainerWriter::encode(
            std::move(forgedSections), forgedContainer, error))
        return fail(error);
    moon::ContainerManifest untouchedManifest;
    untouchedManifest.packageId = "untouched.manifest";
    moon::Module untouchedContainerModule;
    untouchedContainerModule.name = "untouched.module";
    if (moon::ContainerModelCodec::decodeContainer(
            forgedContainer, untouchedManifest,
            untouchedContainerModule, error) ||
        untouchedManifest.packageId != "untouched.manifest" ||
        untouchedContainerModule.name != "untouched.module" ||
        !untouchedContainerModule.declarations.empty())
        return fail("whole-container decoder bypassed Verifier or published partial state");

    auto* sourceFunction = dynamic_cast<moon::FunctionDecl*>(
        source.declarations[0].get());
    if (!sourceFunction || !sourceFunction->controlFlow)
        return fail("source function disappeared before nesting test");
    sourceFunction->typeParams.push_back("T");
    if (moon::ContainerModelCodec::encodeContainer(
            containerManifest, source, canonicalContainerBytes, error) ||
        error.find("generic") == std::string::npos)
        return fail("whole-container encoder accepted a generic recipe");
    sourceFunction->typeParams.clear();
    auto& sourceTerminator =
        sourceFunction->controlFlow->blocks[0].terminator;
    auto originalReturn = std::move(sourceTerminator.operand);
    std::unique_ptr<moon::Expr> nested = std::make_unique<moon::IntLiteralExpr>();
    nested->type = i32Ref;
    for (uint32_t depth = 1;
         depth < moon::ContainerLimits{}.maximumNestingDepth; ++depth) {
        auto unary = std::make_unique<moon::UnaryExpr>();
        unary->op = moon::Operator::Negate;
        unary->type = i32Ref;
        unary->operand = std::move(nested);
        nested = std::move(unary);
    }
    sourceTerminator.operand = std::move(nested);
    if (!moon::ContainerModelCodec::encodeCode(
            source, canonicalCodeBytes, error))
        return fail("code encoder rejected exactly 256 expression levels: " + error);
    auto tooDeep = std::make_unique<moon::UnaryExpr>();
    tooDeep->op = moon::Operator::Negate;
    tooDeep->type = i32Ref;
    tooDeep->operand = std::move(sourceTerminator.operand);
    sourceTerminator.operand = std::move(tooDeep);
    if (moon::ContainerModelCodec::encodeCode(
            source, canonicalCodeBytes, error) ||
        error.find("nesting") == std::string::npos)
        return fail("code encoder accepted 257 expression levels");
    sourceTerminator.operand = std::move(originalReturn);

    auto truncatedExports = exportBytes;
    truncatedExports.pop_back();
    moon::Module interfaceUntouched;
    interfaceUntouched.name = "untouched.module";
    if (moon::ContainerModelCodec::decodeInterfaces(
            importBytes, truncatedExports, interfaceUntouched, error) ||
        !interfaceUntouched.imports.empty() ||
        !interfaceUntouched.exports.empty() ||
        !interfaceUntouched.packageUses.empty())
        return fail("interface decoder accepted truncation or partially mutated output");

    auto truncatedSysmeta = sysmetaBytes;
    truncatedSysmeta.pop_back();
    moon::Module declarationUntouched;
    declarationUntouched.name = "untouched.module";
    if (moon::ContainerModelCodec::decodeDeclarations(
            symbolBytes, contractBytes, truncatedSysmeta,
            declarationUntouched, error) ||
        !declarationUntouched.declarationTable.empty() ||
        !declarationUntouched.metadataSchemas.empty())
        return fail("declaration decoder accepted truncation or partially mutated output");

    auto truncated = typeBytes;
    truncated.pop_back();
    moon::Module untouched;
    untouched.name = "untouched.module";
    if (moon::ContainerModelCodec::decodeTypes(truncated, untouched, error) ||
        untouched.typeTableSealed || !untouched.typeTable.empty())
        return fail("type decoder accepted truncation or partially mutated output");

    auto invalidTypeUtf8 = typeBytes;
    // u32 row count, u32 first TypeId byte length, then its first UTF-8 byte.
    invalidTypeUtf8[8] = 0xff;
    if (moon::ContainerModelCodec::decodeTypes(
            invalidTypeUtf8, untouched, error) ||
        error.find("UTF-8") == std::string::npos)
        return fail("type decoder accepted invalid UTF-8");

    moon::ContainerLimits tiny;
    tiny.maximumStringBytes = 3;
    if (moon::ContainerModelCodec::decodeTypes(
            typeBytes, untouched, error, tiny) ||
        error.find("string") == std::string::npos)
        return fail("type decoder ignored its string resource limit");

    moon::Module outOfOrder;
    outOfOrder.name = source.name;
    outOfOrder.typeTable = source.typeTable;
    outOfOrder.typeTableSealed = true;
    std::swap(outOfOrder.typeTable[0], outOfOrder.typeTable[1]);
    if (moon::ContainerModelCodec::encodeTypes(
            outOfOrder, canonicalTypeBytes, error) ||
        error.find("out of order") == std::string::npos)
        return fail("type encoder accepted a non-canonical table order");

    if (argc == 3 || argc == 6 || argc == 8 || argc == 10) {
        const auto write = [](const char* path,
                              const std::vector<uint8_t>& bytes) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            return output.good();
        };
        if (!write(argv[1], manifestBytes) || !write(argv[2], typeBytes))
            return fail("could not write model-codec oracle fixtures");
        if (argc >= 6 &&
            (!write(argv[3], symbolBytes) ||
             !write(argv[4], contractBytes) ||
             !write(argv[5], sysmetaBytes)))
            return fail("could not write declaration-codec oracle fixtures");
        if (argc >= 8 &&
            (!write(argv[6], importBytes) || !write(argv[7], exportBytes)))
            return fail("could not write interface-codec oracle fixtures");
        if (argc == 10 &&
            (!write(argv[8], codeBytes) || !write(argv[9], containerBytes)))
            return fail("could not write code/container oracle fixtures");
    } else if (argc != 1) {
        return fail("usage: moon-container-model-test [manifest.bin types.bin"
                    " [symbols.bin contracts.bin sysmeta.bin"
                    " [imports.bin exports.bin"
                    " [code.bin module.moon]]]]");
    }

    return 0;
}
