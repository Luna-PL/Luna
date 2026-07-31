#include "CodeGenerator.h"

#include <cstring>
#include <sstream>

#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

namespace {

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

    auto* i8 = llvm::Type::getInt8Ty(*mCtx);
    auto* i32 = mHelpers->i32Ty();
    auto* i64 = llvm::Type::getInt64Ty(*mCtx);
    auto* ptr = llvm::cast<llvm::PointerType>(mHelpers->ptrTy());
    auto* metadataValueType = llvm::StructType::create(*mCtx, "moon.metadata.value");
    metadataValueType->setBody({i8, i64, ptr});
    auto* metadataInstanceType = llvm::StructType::create(
        *mCtx, "moon.metadata.instance");
    metadataInstanceType->setBody({ptr, i64, ptr, i8});
    auto* descriptorType = llvm::StructType::create(
        *mCtx, "moon.declaration.descriptor");
    descriptorType->setBody({i32, ptr, ptr, ptr, i8, i8, i64, ptr, ptr});

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
    for (const auto& record : mProgram->declarationTable) {
        std::vector<const moon::MetadataInstance*> retainedMetadata;
        for (const auto& metadata : record.metadata) {
            if (metadata.retention != moon::Retention::CompileTime)
                retainedMetadata.push_back(&metadata);
        }
        if (record.retention == moon::Retention::CompileTime &&
            retainedMetadata.empty())
            continue;

        std::ostringstream suffixStream;
        suffixStream << std::hex << stableRuntimeId(record.id);
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
                    {llvm::ConstantInt::get(i8, kind),
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
                {cString(metadata.schemaId),
                 llvm::ConstantInt::get(i64, metadata.values.size()),
                 valuesPointer,
                 llvm::ConstantInt::get(
                     i8, static_cast<uint8_t>(metadata.retention))}));
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
        auto* descriptor = llvm::ConstantStruct::get(
            descriptorType,
            {llvm::ConstantInt::get(i32, 1),
             cString(record.id), cString(record.familyId),
             cString(record.linkageName),
             llvm::ConstantInt::get(i8, static_cast<uint8_t>(record.kind)),
             llvm::ConstantInt::get(i8, static_cast<uint8_t>(record.retention)),
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
    auto* registryType = llvm::StructType::get(i64, pointerArrayType);
    auto* registryValue = llvm::ConstantStruct::get(
        registryType,
        {llvm::ConstantInt::get(i64, descriptorPointers.size()), pointerArray});
    std::ostringstream registryName;
    registryName << "__moon_runtime_registry_" << std::hex
                 << stableRuntimeId(mProgram->name);
    auto* registry = new llvm::GlobalVariable(
        *mModule, registryType, true, llvm::GlobalValue::ExternalLinkage,
        registryValue, registryName.str());
    registry->setSection(runtimeSections.registry);
    retainedGlobals.push_back(registry);
    llvm::appendToCompilerUsed(*mModule, retainedGlobals);
}
