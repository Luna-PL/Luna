#include "CodeGenerator.h"
#include "driver/NativeArtifact.h"
#include "runtime/RuntimeDescriptor.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

namespace {

static_assert(
    static_cast<uint32_t>(moon::DeclarationKind::Function) + 1 ==
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Fragment) + 1 ==
        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Struct) + 1 ==
        LUNA_RUNTIME_DECLARATION_STRUCT_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Enum) + 1 ==
        LUNA_RUNTIME_DECLARATION_ENUM_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Trait) + 1 ==
        LUNA_RUNTIME_DECLARATION_TRAIT_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Implementation) + 1 ==
        LUNA_RUNTIME_DECLARATION_IMPLEMENTATION_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::MetadataSchema) + 1 ==
        LUNA_RUNTIME_DECLARATION_METADATA_SCHEMA_V1 &&
    static_cast<uint32_t>(moon::DeclarationKind::Slot) + 1 ==
        LUNA_RUNTIME_DECLARATION_SLOT_V1,
    "Moon declaration kinds must match Runtime descriptor ABI v1");
static_assert(
    static_cast<uint32_t>(moon::Retention::CompileTime) ==
        LUNA_RUNTIME_RETENTION_COMPILE_TIME_V1 &&
    static_cast<uint32_t>(moon::Retention::Runtime) ==
        LUNA_RUNTIME_RETENTION_RUNTIME_V1,
    "Moon retention kinds must match Runtime descriptor ABI v1");

struct MoonRuntimeSectionNames {
    const char* descriptors;
    const char* registry;
};

MoonRuntimeSectionNames moonRuntimeSectionNames() {
    const llvm::Triple host(llvm::sys::getProcessTriple());
    if (host.isOSBinFormatMachO()) {
        // Mach-O section specifications require both a segment and a section;
        // each component is limited to 16 bytes. Keep these stable because a
        // future MoonRuntime loader will enumerate them directly.
        return {"__DATA,__moon_desc", "__DATA,__moon_registry"};
    }
    if (host.isOSBinFormatCOFF()) {
        // '$' suffixes are the conventional COFF subsection spelling and keep
        // all Moon runtime records grouped deterministically by the linker.
        return {".moon$D", ".moon$R"};
    }
    return {".moon.runtime.descriptor", ".moon.runtime.registry"};
}

uint64_t stableRuntimeId(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

void CodeGenerator::emitRuntimeDescriptors() {
    if (!mProgram || !mProgram->features.runtime) return;

    auto* i32 = mHelpers->i32Ty();
    auto* i64 = llvm::Type::getInt64Ty(*mCtx);
    auto* ptr = llvm::cast<llvm::PointerType>(mHelpers->ptrTy());
    auto* metadataValueType = llvm::StructType::create(
        *mCtx, "moon.runtime.metadata.value.v1");
    metadataValueType->setBody({i32, i32, i64, ptr});
    auto* metadataInstanceType = llvm::StructType::create(
        *mCtx, "moon.runtime.metadata.instance.v1");
    metadataInstanceType->setBody(
        {i32, i32, i32, i32, ptr, i64, ptr});
    auto* descriptorType = llvm::StructType::create(
        *mCtx, "moon.runtime.declaration.v1");
    descriptorType->setBody(
        {i32, i32, i32, i32, i32, i32, i32, i32,
         ptr, ptr, ptr, ptr, i64, ptr, ptr});

    std::unordered_map<std::string, llvm::Constant*> strings;
    auto cString = [&](const std::string& text) -> llvm::Constant* {
        auto found = strings.find(text);
        if (found != strings.end()) return found->second;
        auto* initializer = llvm::ConstantDataArray::getString(*mCtx, text, true);
        std::ostringstream name;
        name << "__moon_string_" << std::hex << stableRuntimeId(text);
        auto* global = new llvm::GlobalVariable(
            *mModule, initializer->getType(), true,
            llvm::GlobalValue::PrivateLinkage, initializer, name.str());
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        auto* zero = llvm::ConstantInt::get(i32, 0);
        llvm::Constant* indices[] = {zero, zero};
        auto* address = llvm::ConstantExpr::getInBoundsGetElementPtr(
            initializer->getType(), global, indices);
        strings.emplace(text, address);
        return address;
    };

    const MoonRuntimeSectionNames runtimeSections = moonRuntimeSectionNames();
    std::vector<llvm::GlobalValue*> retainedGlobals;
    std::vector<llvm::Constant*> descriptorPointers;
    std::vector<const moon::DeclarationRecord*> retainedRecords;
    for (const auto& record : mProgram->declarationTable) {
        const bool hasRetainedMetadata = std::any_of(
            record.metadata.begin(), record.metadata.end(),
            [](const moon::MetadataInstance& metadata) {
                return metadata.retention != moon::Retention::CompileTime;
            });
        if (record.retention != moon::Retention::CompileTime ||
            hasRetainedMetadata)
            retainedRecords.push_back(&record);
    }
    std::sort(
        retainedRecords.begin(), retainedRecords.end(),
        [](const moon::DeclarationRecord* left,
           const moon::DeclarationRecord* right) {
            return left->symbolId.value < right->symbolId.value;
        });
    for (const auto* recordPointer : retainedRecords) {
        const auto& record = *recordPointer;
        std::vector<const moon::MetadataInstance*> retainedMetadata;
        for (const auto& metadata : record.metadata) {
            if (metadata.retention != moon::Retention::CompileTime)
                retainedMetadata.push_back(&metadata);
        }
        std::ostringstream suffixStream;
        suffixStream << std::hex << stableRuntimeId(record.symbolId.value);
        const std::string suffix = suffixStream.str();
        std::vector<llvm::Constant*> metadataConstants;
        for (size_t metadataIndex = 0;
             metadataIndex < retainedMetadata.size(); ++metadataIndex) {
            const auto& metadata = *retainedMetadata[metadataIndex];
            std::vector<llvm::Constant*> valueConstants;
            for (const auto& value : metadata.values) {
                uint8_t kind = 0;
                uint64_t payload = 0;
                llvm::Constant* text = llvm::ConstantPointerNull::get(ptr);
                if (auto* integer = std::get_if<int64_t>(&value)) {
                    payload = static_cast<uint64_t>(*integer);
                } else if (auto* floating = std::get_if<double>(&value)) {
                    kind = 1;
                    static_assert(sizeof(payload) == sizeof(*floating));
                    std::memcpy(&payload, floating, sizeof(payload));
                } else if (auto* boolean = std::get_if<bool>(&value)) {
                    kind = 2;
                    payload = *boolean ? 1 : 0;
                } else {
                    kind = 3;
                    text = cString(std::get<std::string>(value));
                }
                valueConstants.push_back(llvm::ConstantStruct::get(
                    metadataValueType,
                    {llvm::ConstantInt::get(i32, kind),
                     llvm::ConstantInt::get(i32, 0),
                     llvm::ConstantInt::get(i64, payload), text}));
            }

            llvm::Constant* valuesPointer = llvm::ConstantPointerNull::get(ptr);
            if (!valueConstants.empty()) {
                auto* arrayType = llvm::ArrayType::get(
                    metadataValueType, valueConstants.size());
                auto* array = llvm::ConstantArray::get(arrayType, valueConstants);
                auto* valuesGlobal = new llvm::GlobalVariable(
                    *mModule, arrayType, true, llvm::GlobalValue::PrivateLinkage,
                    array, "__moon_meta_values_" + suffix + "_" +
                           std::to_string(metadataIndex));
                valuesPointer = valuesGlobal;
            }
            metadataConstants.push_back(llvm::ConstantStruct::get(
                metadataInstanceType,
                {llvm::ConstantInt::get(
                     i32, LUNA_RUNTIME_DESCRIPTOR_ABI_V1),
                 llvm::ConstantInt::get(
                     i32, sizeof(LunaRuntimeMetadataInstanceV1)),
                 llvm::ConstantInt::get(
                     i32, static_cast<uint32_t>(metadata.retention)),
                 llvm::ConstantInt::get(i32, 0),
                 cString(metadata.schemaId),
                 llvm::ConstantInt::get(i64, metadata.values.size()),
                 valuesPointer}));
        }

        llvm::Constant* metadataPointer = llvm::ConstantPointerNull::get(ptr);
        if (!metadataConstants.empty()) {
            auto* arrayType = llvm::ArrayType::get(
                metadataInstanceType, metadataConstants.size());
            auto* array = llvm::ConstantArray::get(arrayType, metadataConstants);
            auto* metadataGlobal = new llvm::GlobalVariable(
                *mModule, arrayType, true, llvm::GlobalValue::PrivateLinkage,
                array, "__moon_metadata_" + suffix);
            metadataPointer = metadataGlobal;
        }

        llvm::Constant* entry = llvm::ConstantPointerNull::get(ptr);
        auto function = mFunctions.find(record.linkageName);
        if (function != mFunctions.end()) entry = function->second;
        const uint32_t flags = entry->isNullValue()
            ? 0 : LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1;
        auto* descriptor = llvm::ConstantStruct::get(
            descriptorType,
            {llvm::ConstantInt::get(
                 i32, LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1),
             llvm::ConstantInt::get(i32, LUNA_RUNTIME_DESCRIPTOR_ABI_V1),
             llvm::ConstantInt::get(
                 i32, sizeof(LunaRuntimeDeclarationDescriptorV1)),
             llvm::ConstantInt::get(
                 i32, static_cast<uint32_t>(record.kind) + 1),
             llvm::ConstantInt::get(i32, flags),
             llvm::ConstantInt::get(
                 i32, static_cast<uint32_t>(record.retention)),
             llvm::ConstantInt::get(i32, 0),
             llvm::ConstantInt::get(i32, 0),
             cString(record.symbolId.value),
             cString(record.contractId.value), cString(record.type.value),
             cString(record.linkageName),
             llvm::ConstantInt::get(i64, retainedMetadata.size()),
             metadataPointer, entry});
        auto* descriptorGlobal = new llvm::GlobalVariable(
            *mModule, descriptorType, true, llvm::GlobalValue::InternalLinkage,
            descriptor, "__moon_descriptor_" + suffix);
        descriptorGlobal->setSection(runtimeSections.descriptors);
        retainedGlobals.push_back(descriptorGlobal);
        descriptorPointers.push_back(descriptorGlobal);
    }

    if (descriptorPointers.empty()) return;
    auto* pointerArrayType = llvm::ArrayType::get(ptr, descriptorPointers.size());
    auto* pointerArray = llvm::ConstantArray::get(pointerArrayType, descriptorPointers);
    std::ostringstream registrySuffix;
    registrySuffix << std::hex << stableRuntimeId(mProgram->name);
    auto* pointerArrayGlobal = new llvm::GlobalVariable(
        *mModule, pointerArrayType, true, llvm::GlobalValue::InternalLinkage,
        pointerArray, "__moon_runtime_descriptors_" + registrySuffix.str());
    auto* registryType = llvm::StructType::create(
        *mCtx, "moon.runtime.registry.v1");
    registryType->setBody({i32, i32, i32, i32, ptr, i64, ptr});
    auto* registryValue = llvm::ConstantStruct::get(
        registryType,
        {llvm::ConstantInt::get(i32, LUNA_RUNTIME_REGISTRY_MAGIC_V1),
         llvm::ConstantInt::get(i32, LUNA_RUNTIME_DESCRIPTOR_ABI_V1),
         llvm::ConstantInt::get(
             i32, sizeof(LunaRuntimeDescriptorRegistryV1)),
         llvm::ConstantInt::get(i32, 0), cString(mProgram->name),
         llvm::ConstantInt::get(i64, descriptorPointers.size()),
         pointerArrayGlobal});
    auto* registry = new llvm::GlobalVariable(
        *mModule, registryType, true, llvm::GlobalValue::ExternalLinkage,
        registryValue,
        luna::runtime::runtimeDescriptorRegistrySymbol(mProgram->name));
    registry->setSection(runtimeSections.registry);
    retainedGlobals.push_back(pointerArrayGlobal);
    retainedGlobals.push_back(registry);
    llvm::appendToCompilerUsed(*mModule, retainedGlobals);
}

bool CodeGenerator::emitNativeProofPlaceholder(
    const std::vector<uint8_t>& record) {
    if (!mModule || record.empty()) {
        error("cannot emit an empty Native proof record");
        return false;
    }
    if (mModule->getNamedGlobal("luna_native_proof_v1")) {
        error("reserved Native proof symbol 'luna_native_proof_v1' is already defined");
        return false;
    }
    auto* initializer = llvm::ConstantDataArray::get(*mCtx, record);
    auto* proof = new llvm::GlobalVariable(
        *mModule, initializer->getType(), true,
        llvm::GlobalValue::ExternalLinkage, initializer,
        "luna_native_proof_v1");
    const llvm::Triple host(llvm::sys::getProcessTriple());
    if (host.isOSBinFormatMachO())
        proof->setSection("__DATA,__luna_proof");
    else if (host.isOSBinFormatCOFF())
        proof->setSection(".luna$proof");
    else
        proof->setSection(".luna.native.proof");
    if (host.isOSBinFormatCOFF())
        proof->setDLLStorageClass(
            llvm::GlobalValue::DLLExportStorageClass);
    llvm::appendToCompilerUsed(*mModule, {proof});
    return true;
}

bool CodeGenerator::emitNativeLibraryDescriptor(
    const std::string& packageId, const std::string& packageVersion,
    const std::string& targetAbi, const std::string& compilerIdentity,
    const std::vector<luna::driver::NativeExportSpec>& exports) {
    if (!mModule ||
        mModule->getFunction("luna_native_library_descriptor_v1")) {
        error("reserved Native descriptor symbol is already defined");
        return false;
    }
    auto* i32 = llvm::Type::getInt32Ty(*mCtx);
    auto* i64 = llvm::Type::getInt64Ty(*mCtx);
    auto* ptr = llvm::PointerType::getUnqual(*mCtx);
    size_t stringIndex = 0;
    auto cString = [&](const std::string& value) -> llvm::Constant* {
        auto* initializer = llvm::ConstantDataArray::getString(
            *mCtx, value, true);
        auto* global = new llvm::GlobalVariable(
            *mModule, initializer->getType(), true,
            llvm::GlobalValue::PrivateLinkage, initializer,
            "__luna_native_string_" + std::to_string(stringIndex++));
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return global;
    };

    auto* exportType = llvm::StructType::create(
        *mCtx, "luna.native.export.v1");
    exportType->setBody({i32, i32, i32, i32, ptr, ptr, ptr, ptr});
    std::vector<llvm::Constant*> exportValues;
    for (const auto& descriptor : exports) {
        llvm::Constant* entry = llvm::ConstantPointerNull::get(ptr);
        if ((descriptor.flags & LUNA_NATIVE_EXPORT_CALLABLE_V1) != 0) {
            auto* function = mModule->getFunction(descriptor.linkageName);
            if (!function) {
                error("Native callable export '" + descriptor.linkageName +
                      "' has no generated entry");
                return false;
            }
            entry = function;
        }
        exportValues.push_back(llvm::ConstantStruct::get(
            exportType,
            {llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_ABI_V1),
             llvm::ConstantInt::get(i32,
                                    sizeof(LunaNativeExportDescriptorV1)),
             llvm::ConstantInt::get(i32, descriptor.declarationKind),
             llvm::ConstantInt::get(i32, descriptor.flags),
             cString(descriptor.symbolId), cString(descriptor.contractId),
             cString(descriptor.linkageName), entry}));
    }
    llvm::Constant* exportsPointer = llvm::ConstantPointerNull::get(ptr);
    llvm::GlobalVariable* exportsGlobal = nullptr;
    if (!exportValues.empty()) {
        auto* arrayType = llvm::ArrayType::get(exportType, exportValues.size());
        exportsGlobal = new llvm::GlobalVariable(
            *mModule, arrayType, true, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrayType, exportValues),
            "__luna_native_exports_v1");
        exportsPointer = exportsGlobal;
    }

    auto* descriptorType = llvm::StructType::create(
        *mCtx, "luna.native.library.v1");
    descriptorType->setBody(
        {i32, i32, i32, i32, ptr, ptr, ptr, ptr, i64, ptr});
    auto* descriptor = new llvm::GlobalVariable(
        *mModule, descriptorType, true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantStruct::get(
            descriptorType,
            {llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_MAGIC_V1),
             llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_ABI_V1),
             llvm::ConstantInt::get(i32,
                                    sizeof(LunaNativeLibraryDescriptorV1)),
             llvm::ConstantInt::get(i32, 0), cString(packageId),
             cString(packageVersion), cString(targetAbi),
             cString(compilerIdentity),
             llvm::ConstantInt::get(i64, exportValues.size()),
             exportsPointer}),
        "__luna_native_library_v1");
    const llvm::Triple host(llvm::sys::getProcessTriple());
    if (host.isOSBinFormatMachO())
        descriptor->setSection("__DATA,__luna_desc");
    else if (host.isOSBinFormatCOFF())
        descriptor->setSection(".luna$desc");
    else
        descriptor->setSection(".luna.native.descriptor");

    auto* queryType = llvm::FunctionType::get(ptr, false);
    auto* query = llvm::Function::Create(
        queryType, llvm::GlobalValue::ExternalLinkage,
        "luna_native_library_descriptor_v1", *mModule);
    if (host.isOSBinFormatCOFF())
        query->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    auto* block = llvm::BasicBlock::Create(*mCtx, "entry", query);
    llvm::IRBuilder<> builder(block);
    builder.CreateRet(descriptor);
    std::vector<llvm::GlobalValue*> retained = {descriptor, query};
    if (exportsGlobal) retained.push_back(exportsGlobal);
    llvm::appendToCompilerUsed(*mModule, retained);
    return true;
}
