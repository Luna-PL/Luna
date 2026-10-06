#include "CodeGenerator.h"
#include "driver/NativeArtifact.h"
#include "driver/NativeTypedDescriptor.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragmentABI.h"

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

bool isExportedRuntimeControl(
    const moon::Module& module, const moon::DeclarationRecord& record) {
    if (record.kind != moon::DeclarationKind::Slot &&
        record.kind != moon::DeclarationKind::Fragment)
        return false;
    const moon::DeclarationRef reference{record.symbolId, record.contractId};
    return std::any_of(
        module.exports.begin(), module.exports.end(),
        [&](const moon::ExportRecord& exported) {
            return exported.declaration == reference;
        });
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
    auto* fragmentDescriptorType = llvm::StructType::create(
        *mCtx, "moon.runtime.fragment.v1");
    fragmentDescriptorType->setBody({
        i32, i32, i32, i32, i32, i32, i32, i32,
        ptr, ptr, ptr, ptr, ptr, i64, i64, ptr, ptr, i64, i64,
        ptr, ptr, ptr});

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
            hasRetainedMetadata ||
            isExportedRuntimeControl(*mProgram, record))
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
        bool fragmentExecutable = false;
        if (record.kind == moon::DeclarationKind::Fragment &&
            record.runtimeEntry.complete() &&
            record.controlTarget.complete()) {
            const auto* helperRecord = mProgram->findDeclaration(
                record.runtimeEntry);
            const auto* slotRecord = mProgram->findDeclaration(
                record.controlTarget);
            const auto* environmentRecord = mProgram->findType(
                record.environmentType);
            const auto* argumentsRecord = mProgram->findType(
                record.controlArgumentsType);
            auto helper = helperRecord
                ? mFunctions.find(helperRecord->linkageName)
                : mFunctions.end();
            if (!helperRecord || !slotRecord || !environmentRecord ||
                !argumentsRecord || helper == mFunctions.end()) {
                error("exported Fragment '" + record.sourceName +
                      "' has an incomplete runtime entry");
            } else {
                const TypePtr environmentType = resolveType(
                    record.environmentType);
                const TypePtr argumentsType = resolveType(
                    record.controlArgumentsType);
                auto* environmentLLVM = llvm::dyn_cast_or_null<llvm::StructType>(
                    mHelpers->toLLVMType(environmentType));
                auto* argumentsLLVM = llvm::dyn_cast_or_null<llvm::StructType>(
                    mHelpers->toLLVMType(argumentsType));
                const bool captureFree = environmentRecord->fields.empty();
                llvm::Constant* factory = llvm::ConstantPointerNull::get(ptr);
                llvm::Constant* destroy = llvm::ConstantPointerNull::get(ptr);

                if (!captureFree && environmentLLVM) {
                    auto* factoryType = llvm::FunctionType::get(
                        i32, {ptr, ptr}, false);
                    auto* factoryFunction = llvm::Function::Create(
                        factoryType, llvm::GlobalValue::PrivateLinkage,
                        "__moon_fragment_factory_" + suffix, *mModule);
                    auto argumentsIterator = factoryFunction->arg_begin();
                    llvm::Value* factoryArguments = &*argumentsIterator++;
                    llvm::Value* outputEnvironment = &*argumentsIterator;
                    auto* factoryEntry = llvm::BasicBlock::Create(
                        *mCtx, "entry", factoryFunction);
                    auto* factoryInvalid = llvm::BasicBlock::Create(
                        *mCtx, "invalid", factoryFunction);
                    auto* factoryConstruct = llvm::BasicBlock::Create(
                        *mCtx, "construct", factoryFunction);
                    auto* factoryCopy = llvm::BasicBlock::Create(
                        *mCtx, "copy", factoryFunction);
                    llvm::IRBuilder<> builder(factoryEntry);
                    auto* valid = builder.CreateAnd(
                        builder.CreateIsNotNull(factoryArguments),
                        builder.CreateIsNotNull(outputEnvironment));
                    builder.CreateCondBr(valid, factoryConstruct, factoryInvalid);
                    builder.SetInsertPoint(factoryInvalid);
                    builder.CreateRet(llvm::ConstantInt::getSigned(i32, -1));
                    builder.SetInsertPoint(factoryConstruct);
                    auto allocation = mModule->getOrInsertFunction(
                        "rt_alloc", ptr, mHelpers->sizeTy(),
                        mHelpers->sizeTy());
                    auto* environment = builder.CreateCall(
                        allocation,
                        {llvm::ConstantInt::get(
                             mHelpers->sizeTy(), environmentRecord->valueSize),
                         llvm::ConstantInt::get(
                             mHelpers->sizeTy(), environmentRecord->valueAlignment)},
                        "environment");
                    builder.CreateCondBr(
                        builder.CreateIsNotNull(environment),
                        factoryCopy, factoryInvalid);
                    builder.SetInsertPoint(factoryCopy);
                    builder.CreateMemCpy(
                        environment, llvm::Align(environmentRecord->valueAlignment),
                        factoryArguments,
                        llvm::Align(environmentRecord->valueAlignment),
                        environmentRecord->valueSize);
                    builder.CreateStore(environment, outputEnvironment);
                    builder.CreateRet(llvm::ConstantInt::get(i32, 0));
                    factory = factoryFunction;
                    retainedGlobals.push_back(factoryFunction);

                    auto* destroyType = llvm::FunctionType::get(
                        mHelpers->voidTy(), {ptr}, false);
                    auto* destroyFunction = llvm::Function::Create(
                        destroyType, llvm::GlobalValue::PrivateLinkage,
                        "__moon_fragment_destroy_" + suffix, *mModule);
                    auto* destroyEntry = llvm::BasicBlock::Create(
                        *mCtx, "entry", destroyFunction);
                    llvm::IRBuilder<> destroyBuilder(destroyEntry);
                    auto deallocation = mModule->getOrInsertFunction(
                        "rt_dealloc", mHelpers->voidTy(), ptr,
                        mHelpers->sizeTy(), mHelpers->sizeTy());
                    destroyBuilder.CreateCall(
                        deallocation,
                        {&*destroyFunction->arg_begin(),
                         llvm::ConstantInt::get(
                             mHelpers->sizeTy(), environmentRecord->valueSize),
                         llvm::ConstantInt::get(
                             mHelpers->sizeTy(), environmentRecord->valueAlignment)});
                    destroyBuilder.CreateRetVoid();
                    destroy = destroyFunction;
                    retainedGlobals.push_back(destroyFunction);
                }

                if (!environmentLLVM || !argumentsLLVM) {
                    error("exported Fragment '" + record.sourceName +
                          "' does not use record ABI layouts");
                } else {
                    auto* executeType = llvm::FunctionType::get(
                        mHelpers->voidTy(), {ptr, ptr}, false);
                    auto* executeFunction = llvm::Function::Create(
                        executeType, llvm::GlobalValue::PrivateLinkage,
                        "__moon_fragment_execute_" + suffix, *mModule);
                    auto executeArguments = executeFunction->arg_begin();
                    llvm::Value* environment = &*executeArguments++;
                    llvm::Value* activation = &*executeArguments;
                    auto* executeEntry = llvm::BasicBlock::Create(
                        *mCtx, "entry", executeFunction);
                    auto* executeBody = llvm::BasicBlock::Create(
                        *mCtx, "invoke", executeFunction);
                    auto* executeReturn = llvm::BasicBlock::Create(
                        *mCtx, "return", executeFunction);
                    llvm::IRBuilder<> builder(executeEntry);
                    auto accessor = mModule->getOrInsertFunction(
                        "luna_runtime_fragment_activation_arguments_v1",
                        ptr, ptr, ptr, ptr, ptr, i64, i64);
                    auto* slotArguments = builder.CreateCall(
                        accessor,
                        {activation, cString(slotRecord->symbolId.value),
                         cString(slotRecord->contractId.value),
                         cString(argumentsRecord->abiLayoutId.value),
                         llvm::ConstantInt::get(i64, argumentsRecord->valueSize),
                         llvm::ConstantInt::get(
                             i64, argumentsRecord->valueAlignment)},
                        "slot.arguments");
                    llvm::Value* valid = llvm::ConstantInt::getTrue(*mCtx);
                    if (argumentsRecord->valueSize != 0)
                        valid = builder.CreateIsNotNull(slotArguments);
                    if (!captureFree)
                        valid = builder.CreateAnd(
                            valid, builder.CreateIsNotNull(environment));
                    builder.CreateCondBr(valid, executeBody, executeReturn);
                    builder.SetInsertPoint(executeBody);
                    std::vector<llvm::Value*> helperArguments;
                    helperArguments.reserve(
                        environmentRecord->fields.size() +
                        argumentsRecord->fields.size() + 1);
                    for (size_t index = 0;
                         index < environmentRecord->fields.size(); ++index) {
                        auto* address = builder.CreateStructGEP(
                            environmentLLVM, environment,
                            static_cast<unsigned>(index));
                        helperArguments.push_back(builder.CreateLoad(
                            environmentLLVM->getElementType(index), address));
                    }
                    for (size_t index = 0;
                         index < argumentsRecord->fields.size(); ++index) {
                        auto* address = builder.CreateStructGEP(
                            argumentsLLVM, slotArguments,
                            static_cast<unsigned>(index));
                        helperArguments.push_back(builder.CreateLoad(
                            argumentsLLVM->getElementType(index), address));
                    }
                    helperArguments.push_back(activation);
                    if (helper->second->arg_size() != helperArguments.size()) {
                        error("runtime Fragment helper parameter count is inconsistent");
                    } else {
                        builder.CreateCall(helper->second, helperArguments);
                    }
                    builder.CreateBr(executeReturn);
                    builder.SetInsertPoint(executeReturn);
                    builder.CreateRetVoid();
                    retainedGlobals.push_back(executeFunction);

                    auto* fragmentDescriptor = new llvm::GlobalVariable(
                        *mModule, fragmentDescriptorType, true,
                        llvm::GlobalValue::PrivateLinkage,
                        llvm::ConstantStruct::get(
                            fragmentDescriptorType,
                            {llvm::ConstantInt::get(
                                 i32, LUNA_RUNTIME_FRAGMENT_MAGIC_V1),
                             llvm::ConstantInt::get(
                                 i32, LUNA_RUNTIME_FRAGMENT_ABI_V1),
                             llvm::ConstantInt::get(
                                 i32, sizeof(LunaRuntimeFragmentDescriptorV1)),
                             llvm::ConstantInt::get(
                                 i32, captureFree
                                     ? LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1 : 0),
                             llvm::ConstantInt::get(i32, 0),
                             llvm::ConstantInt::get(i32, 0),
                             llvm::ConstantInt::get(i32, 0),
                             llvm::ConstantInt::get(i32, 0),
                             cString(record.symbolId.value),
                             cString(record.contractId.value),
                             cString(slotRecord->symbolId.value),
                             cString(slotRecord->contractId.value),
                             cString(argumentsRecord->abiLayoutId.value),
                             llvm::ConstantInt::get(
                                 i64, argumentsRecord->valueSize),
                             llvm::ConstantInt::get(
                                 i64, argumentsRecord->valueAlignment),
                             cString(captureFree
                                 ? std::string{} : record.environmentType.value),
                             cString(environmentRecord->abiLayoutId.value),
                             llvm::ConstantInt::get(
                                 i64, captureFree ? 0 : environmentRecord->valueSize),
                             llvm::ConstantInt::get(
                                 i64, captureFree ? 1 : environmentRecord->valueAlignment),
                             factory, destroy, executeFunction}),
                        "__moon_fragment_descriptor_" + suffix);
                    retainedGlobals.push_back(fragmentDescriptor);
                    entry = fragmentDescriptor;
                    fragmentExecutable = true;
                }
            }
        }
        // Runtime metadata retains only the declaration identity needed to
        // host the attachment. It must not accidentally publish a callable
        // entry for an otherwise compile-time declaration.
        if (!fragmentExecutable &&
            record.retention != moon::Retention::CompileTime) {
            auto function = mFunctions.find(record.linkageName);
            if (function != mFunctions.end()) entry = function->second;
        }
        uint32_t flags = entry->isNullValue()
            ? 0 : LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1;
        if ((flags & LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) != 0) {
            const auto function = mProgram->functionsBySymbol.find(
                record.linkageName);
            if (function != mProgram->functionsBySymbol.end() &&
                function->second &&
                function->second->requiresFragmentContext)
                flags |= LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1;
        }
        if (fragmentExecutable) {
            flags &= ~LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1;
            flags |= LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1;
        }
        if (isExportedRuntimeControl(*mProgram, record))
            flags |= LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1;
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
        mModule->getFunction("luna_native_library_descriptor_v1") ||
        mModule->getFunction("luna_native_library_descriptor_v2")) {
        error("reserved Native descriptor symbol is already defined");
        return false;
    }
    // V1 has no entry ABI profile. Cross-check each requested row against the
    // generated module's public declarations before placing a raw address in
    // the registry.
    if (!mProgram || mProgram->name != packageId) {
        error("Native v1 descriptor has no matching sealed package");
        return false;
    }
    std::vector<luna::driver::NativeExportSpec> typedExports;
    for (const auto& exported : exports) {
        const auto record = std::find_if(
            mProgram->declarationTable.begin(), mProgram->declarationTable.end(),
            [&](const moon::DeclarationRecord& candidate) {
                return candidate.symbolId.value == exported.symbolId &&
                    candidate.contractId.value == exported.contractId;
            });
        const bool publicRecord = record != mProgram->declarationTable.end() &&
            std::any_of(mProgram->exports.begin(), mProgram->exports.end(),
                [&](const moon::ExportRecord& candidate) {
                    return candidate.declaration == moon::DeclarationRef{
                        record->symbolId, record->contractId};
                });
        if (!publicRecord ||
            exported.declarationKind != static_cast<uint32_t>(record->kind) + 1 ||
            exported.linkageName != record->linkageName ||
            exported.flags != (record->kind == moon::DeclarationKind::Function
                ? LUNA_NATIVE_EXPORT_CALLABLE_V1 : 0)) {
            error("Native v1 export row differs from its sealed public declaration");
            return false;
        }
        if (record->kind == moon::DeclarationKind::Function) {
            const moon::FunctionDecl* function = nullptr;
            for (const auto& declaration : mProgram->declarations)
                if (declaration && declaration->declarationId == record->id) {
                    function = dynamic_cast<const moon::FunctionDecl*>(
                        declaration.get());
                    break;
                }
            if (!function || !function->isExported ||
                function->packageId != packageId || function->isExtern ||
                function->requiresFragmentContext) {
                error("Native v1 callable export requires an unsupported entry ABI");
                return false;
            }
            const auto* signature = mProgram->findType(record->type);
            if (!signature || signature->kind != TypeKind::Function ||
                signature->parameterTypeIds.size() != function->params.size() ||
                signature->returnTypeId != function->returnType) {
                error("Native v1 callable entry differs from its frozen function signature");
                return false;
            }
            std::vector<llvm::Type*> parameters;
            for (size_t index = 0; index < function->params.size(); ++index) {
                if (signature->parameterTypeIds[index] !=
                    function->params[index].type) {
                    error("Native v1 callable entry differs from its frozen function signature");
                    return false;
                }
                const TypePtr type = resolveType(signature->parameterTypeIds[index]);
                if (!type) {
                    error("Native v1 callable entry has an unresolved parameter type");
                    return false;
                }
                parameters.push_back(mHelpers->toLLVMType(type));
            }
            const TypePtr result = resolveType(signature->returnTypeId);
            auto* body = mModule->getFunction(exported.linkageName);
            if (!result || !body || body->isDeclaration() ||
                body->getCallingConv() != llvm::CallingConv::C ||
                body->getFunctionType() != llvm::FunctionType::get(
                    mHelpers->toLLVMType(result), parameters, false)) {
                error("Native v1 callable entry differs from its generated LLVM signature");
                return false;
            }
            if (signature->parameterTypeIds.empty() &&
                signature->returnTypeId == function->returnType &&
                result->kind == TypeKind::I32 &&
                body->getFunctionType() == llvm::FunctionType::get(
                    llvm::Type::getInt32Ty(*mCtx), false))
                typedExports.push_back(exported);
        }
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

    auto* typedExportType = llvm::StructType::create(
        *mCtx, "luna.native.export.v2");
    typedExportType->setBody(
        {i32, i32, i32, i32, i32, i32, ptr, ptr, ptr, ptr});
    std::vector<llvm::Constant*> typedValues;
    std::vector<std::string> canonicalTypedRows;
    for (const auto& exported : typedExports) {
        auto* body = mModule->getFunction(exported.linkageName);
        typedValues.push_back(llvm::ConstantStruct::get(
            typedExportType,
            {llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_ABI_V2),
             llvm::ConstantInt::get(i32, sizeof(LunaNativeExportDescriptorV2)),
             llvm::ConstantInt::get(i32, exported.declarationKind),
             llvm::ConstantInt::get(i32, exported.flags),
             llvm::ConstantInt::get(i32, LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1),
             llvm::ConstantInt::get(i32, 0),
             cString(exported.symbolId), cString(exported.contractId),
             cString(exported.linkageName), body}));
        canonicalTypedRows.push_back(luna::driver::canonicalNativeTypedExport(
            exported.declarationKind, exported.flags,
            LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1,
            exported.symbolId, exported.contractId, exported.linkageName));
    }
    llvm::Constant* typedExportsPointer = llvm::ConstantPointerNull::get(ptr);
    if (!typedValues.empty()) {
        auto* arrayType = llvm::ArrayType::get(typedExportType, typedValues.size());
        auto* rows = new llvm::GlobalVariable(
            *mModule, arrayType, true, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrayType, typedValues),
            "__luna_native_exports_v2");
        typedExportsPointer = rows;
        retained.push_back(rows);
    }
    const auto typedDigest = luna::driver::digestNativeTypedExports(
        std::move(canonicalTypedRows));
    auto* digestType = llvm::ArrayType::get(
        llvm::Type::getInt8Ty(*mCtx), LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V2);
    auto* typedDescriptorType = llvm::StructType::create(
        *mCtx, "luna.native.library.v2");
    typedDescriptorType->setBody(
        {i32, i32, i32, i32, ptr, ptr, ptr, ptr, i64, ptr, digestType});
    auto* typedDescriptor = new llvm::GlobalVariable(
        *mModule, typedDescriptorType, true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantStruct::get(
            typedDescriptorType,
            {llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_MAGIC_V2),
             llvm::ConstantInt::get(i32, LUNA_NATIVE_DESCRIPTOR_ABI_V2),
             llvm::ConstantInt::get(i32, sizeof(LunaNativeLibraryDescriptorV2)),
             llvm::ConstantInt::get(i32, 0), cString(packageId),
             cString(packageVersion), cString(targetAbi),
             cString(compilerIdentity),
             llvm::ConstantInt::get(i64, typedValues.size()),
             typedExportsPointer,
             llvm::ConstantDataArray::get(
                 *mCtx, llvm::ArrayRef<uint8_t>(typedDigest))}),
        "__luna_native_library_v2");
    if (host.isOSBinFormatMachO())
        typedDescriptor->setSection("__DATA,__luna_desc2");
    else if (host.isOSBinFormatCOFF())
        typedDescriptor->setSection(".luna$desc2");
    else
        typedDescriptor->setSection(".luna.native.descriptor.v2");
    auto* typedQuery = llvm::Function::Create(
        queryType, llvm::GlobalValue::ExternalLinkage,
        "luna_native_library_descriptor_v2", *mModule);
    if (host.isOSBinFormatCOFF())
        typedQuery->setDLLStorageClass(
            llvm::GlobalValue::DLLExportStorageClass);
    auto* typedBlock = llvm::BasicBlock::Create(*mCtx, "entry", typedQuery);
    llvm::IRBuilder<> typedBuilder(typedBlock);
    typedBuilder.CreateRet(typedDescriptor);
    retained.push_back(typedDescriptor);
    retained.push_back(typedQuery);
    llvm::appendToCompilerUsed(*mModule, retained);
    return true;
}
