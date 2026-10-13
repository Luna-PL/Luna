#include "driver/NativeArtifact.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/BinaryFormat/MachO.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/MachO.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SHA256.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <utility>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif
#endif

#include "driver/NativeArtifactInternal.h"
#include "driver/NativeTypedDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"

namespace luna::driver {

using namespace native_artifact_detail;

namespace {

#ifndef _WIN32
bool writeAllStagingBytes(int fd, const std::vector<uint8_t>& bytes,
                          std::string& error) {
    size_t written = 0;
    while (written < bytes.size()) {
        const size_t remaining = bytes.size() - written;
        const size_t chunk = std::min(
            remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write(fd, bytes.data() + written, chunk);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            error = "cannot write private Native staging image";
            return false;
        }
        written += static_cast<size_t>(result);
    }
    if (::fsync(fd) != 0) {
        error = "cannot flush private Native staging image";
        return false;
    }
    return true;
}
#endif

struct StagedNativeImage {
    intptr_t handle = -1;
    std::string path;
    std::string directory;
};

void releaseStagedImage(StagedNativeImage& image) noexcept {
#ifdef _WIN32
    if (image.handle != -1)
        CloseHandle(reinterpret_cast<HANDLE>(image.handle));
    if (!image.path.empty()) {
        SetFileAttributesA(image.path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(image.path.c_str());
    }
#else
    if (image.handle != -1) ::close(static_cast<int>(image.handle));
    if (!image.path.empty() && image.directory.size() != 0)
        ::unlink(image.path.c_str());
    if (!image.directory.empty()) {
        ::chmod(image.directory.c_str(), 0700);
        ::rmdir(image.directory.c_str());
    }
#endif
    image = {};
}

#ifdef _WIN32
bool stageNativeImage(const std::vector<uint8_t>& bytes,
                      StagedNativeImage& image, std::string& error) {
    char tempDirectory[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathA(MAX_PATH, tempDirectory);
    if (length == 0 || length > MAX_PATH) {
        error = "cannot locate the Windows temporary directory";
        return false;
    }
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const std::string path = std::string(tempDirectory) +
            "luna-native-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" +
            std::to_string(attempt) + ".dll";
        HANDLE file = CreateFileA(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            error = "cannot create a locked Windows Native staging image";
            return false;
        }
        size_t written = 0;
        bool ok = true;
        while (written < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
                bytes.size() - written,
                static_cast<size_t>(std::numeric_limits<DWORD>::max())));
            DWORD amount = 0;
            if (!WriteFile(file, bytes.data() + written, chunk, &amount,
                           nullptr) || amount == 0) {
                ok = false;
                break;
            }
            written += amount;
        }
        if (ok) ok = FlushFileBuffers(file) != 0;
        if (!ok) {
            CloseHandle(file);
            DeleteFileA(path.c_str());
            error = "cannot write the locked Windows Native staging image";
            return false;
        }
        CloseHandle(file);
        if (!SetFileAttributesA(
                path.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_TEMPORARY)) {
            DeleteFileA(path.c_str());
            error = "cannot make the Windows Native staging image read-only";
            return false;
        }
        file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(path.c_str());
            error = "cannot lock the Windows Native staging image";
            return false;
        }
        image.handle = reinterpret_cast<intptr_t>(file);
        image.path = path;
        return true;
    }
    error = "cannot allocate a unique Windows Native staging name";
    return false;
}
#else
bool stagePrivatePosixFile(const std::vector<uint8_t>& bytes,
                           StagedNativeImage& image, std::string& error) {
    const std::string pattern =
        (std::filesystem::temp_directory_path() / "luna-native-XXXXXX").string();
    std::vector<char> mutablePattern(pattern.begin(), pattern.end());
    mutablePattern.push_back('\0');
    char* created = ::mkdtemp(mutablePattern.data());
    if (!created) {
        error = "cannot create a private Native staging directory";
        return false;
    }
    image.directory = created;
    image.path = (std::filesystem::path(image.directory) / "image").string();
    const int fd = ::open(image.path.c_str(), O_CREAT | O_EXCL | O_RDWR
#ifdef O_CLOEXEC
                          | O_CLOEXEC
#endif
                          , 0600);
    if (fd < 0) {
        error = "cannot create a private Native staging image";
        releaseStagedImage(image);
        return false;
    }
    image.handle = fd;
    if (!writeAllStagingBytes(fd, bytes, error) ||
        ::fchmod(fd, 0400) != 0 || ::chmod(image.directory.c_str(), 0500) != 0) {
        if (error.empty()) error = "cannot lock the Native staging image";
        releaseStagedImage(image);
        return false;
    }
    return true;
}

bool stageNativeImage(const std::vector<uint8_t>& bytes,
                      StagedNativeImage& image, std::string& error) {
#if defined(__linux__) && defined(SYS_memfd_create)
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create, "luna-native-verified", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd >= 0) {
        image.handle = fd;
        image.path = "/proc/self/fd/" + std::to_string(fd);
        if (!writeAllStagingBytes(fd, bytes, error)) {
            releaseStagedImage(image);
            return false;
        }
        const int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
        if (::fcntl(fd, F_ADD_SEALS, seals) != 0) {
            error = "cannot seal the Linux Native staging image";
            releaseStagedImage(image);
            return false;
        }
        return true;
    }
#endif
    return stagePrivatePosixFile(bytes, image, error);
}
#endif

void* openNativeImage(const std::string& path, std::string& error) {
#ifdef _WIN32
    HMODULE library = LoadLibraryA(path.c_str());
    if (!library)
        error = "cannot load verified Native image: Windows loader error " +
            std::to_string(GetLastError());
    return reinterpret_cast<void*>(library);
#else
    dlerror();
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* loaderError = dlerror();
        error = "cannot load verified Native image: " +
            std::string(loaderError ? loaderError : "unknown loader error");
    }
    return library;
#endif
}

void closeNativeImage(void* library) noexcept {
    if (!library) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

void* loadNativeSymbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(
        reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

bool nativeAddressInStagedImage(void* library, const std::string& stagedPath,
                                const void* address) {
    if (!address) return false;
#ifdef _WIN32
    (void)stagedPath;
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(address, &region, sizeof(region)) == sizeof(region) &&
        region.Type == MEM_IMAGE &&
        region.AllocationBase == library;
#else
    (void)library;
    Dl_info origin{};
    struct stat staged{}, resolved{};
    return dladdr(address, &origin) != 0 && origin.dli_fname &&
        ::stat(stagedPath.c_str(), &staged) == 0 &&
        ::stat(origin.dli_fname, &resolved) == 0 &&
        staged.st_dev == resolved.st_dev && staged.st_ino == resolved.st_ino;
#endif
}

bool descriptorString(const char* source, std::string& value) {
    if (!source) return false;
    size_t length = 0;
    while (length < MaxNativeDescriptorString && source[length] != '\0')
        ++length;
    if (length == 0 || length == MaxNativeDescriptorString) return false;
    value.assign(source, length);
    return value.find_first_of("\r\n\t") == std::string::npos &&
        validNativeDescriptorUtf8(value);
}

bool validateNativeDescriptor(
    void* handle, const NativeProofInfo& proof,
    const LunaNativeLibraryDescriptorV1*& descriptor,
    std::vector<std::string>& proofExports, std::string& error) {
    auto* rawQuery = loadNativeSymbol(
        handle, "luna_native_library_descriptor_v1");
    if (!rawQuery) {
        error = "verified Native image has no v1 descriptor query";
        return false;
    }
    const auto query = reinterpret_cast<LunaNativeLibraryDescriptorFnV1>(rawQuery);
    descriptor = query();
    if (!descriptor || descriptor->magic != LUNA_NATIVE_DESCRIPTOR_MAGIC_V1 ||
        descriptor->abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V1 ||
        descriptor->struct_size != sizeof(LunaNativeLibraryDescriptorV1) ||
        descriptor->reserved_zero != 0) {
        error = "verified Native image returned an invalid library descriptor";
        return false;
    }
    std::string packageId;
    std::string packageVersion;
    std::string targetAbi;
    std::string compilerIdentity;
    if (!descriptorString(descriptor->package_id, packageId) ||
        !descriptorString(descriptor->package_version, packageVersion) ||
        !descriptorString(descriptor->target_abi, targetAbi) ||
        !descriptorString(descriptor->compiler_identity, compilerIdentity) ||
        packageId != proof.packageId || packageVersion != proof.packageVersion ||
        targetAbi != proof.targetAbi || compilerIdentity != proof.compilerIdentity) {
        error = "Native library descriptor identity does not match its proof";
        return false;
    }
    if (descriptor->export_count > MaxNativeExportCount ||
        (descriptor->export_count != 0 && !descriptor->exports)) {
        error = "Native library descriptor has an invalid export table";
        return false;
    }
    proofExports.reserve(static_cast<size_t>(descriptor->export_count));
    std::set<std::string> symbolIds;
    std::set<std::string> callableLinkages;
    for (uint64_t index = 0; index < descriptor->export_count; ++index) {
        const auto& exported = descriptor->exports[index];
        NativeExportSpec spec;
        spec.declarationKind = exported.declaration_kind;
        spec.flags = exported.flags;
        if (exported.abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V1 ||
            exported.struct_size != sizeof(LunaNativeExportDescriptorV1) ||
            spec.declarationKind < LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
            spec.declarationKind > LUNA_NATIVE_DECLARATION_SLOT_V1 ||
            (spec.flags & ~LUNA_NATIVE_EXPORT_CALLABLE_V1) != 0 ||
            !descriptorString(exported.symbol_id, spec.symbolId) ||
            !descriptorString(exported.contract_id, spec.contractId) ||
            !descriptorString(exported.linkage_name, spec.linkageName)) {
            error = "Native library descriptor contains an invalid export row";
            return false;
        }
        const bool callable =
            (spec.flags & LUNA_NATIVE_EXPORT_CALLABLE_V1) != 0;
        if (callable != (spec.declarationKind ==
                         LUNA_NATIVE_DECLARATION_FUNCTION_V1) ||
            callable != (exported.entry != nullptr) ||
            !symbolIds.insert(spec.symbolId).second ||
            (callable && !callableLinkages.insert(spec.linkageName).second)) {
            error = "Native library descriptor contains an ambiguous export row";
            return false;
        }
        proofExports.push_back(canonicalNativeExport(spec));
    }
    return true;
}

bool validateNativeDescriptorV2(
    void* handle, const NativeProofInfo& proof,
    const LunaNativeLibraryDescriptorV1* v1,
    const LunaNativeLibraryDescriptorV2*& descriptor, std::string& error) {
    auto* rawQuery = loadNativeSymbol(
        handle, "luna_native_library_descriptor_v2");
    if (!rawQuery) return true; // Older, valid V1 artifacts remain loadable.
    const auto query = reinterpret_cast<LunaNativeLibraryDescriptorFnV2>(rawQuery);
    descriptor = query();
    if (!descriptor || descriptor->magic != LUNA_NATIVE_DESCRIPTOR_MAGIC_V2 ||
        descriptor->abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V2 ||
        descriptor->struct_size != sizeof(LunaNativeLibraryDescriptorV2) ||
        descriptor->reserved_zero != 0) {
        error = "verified Native image returned an invalid v2 library descriptor";
        return false;
    }
    std::string packageId, packageVersion, targetAbi, compilerIdentity;
    if (!descriptorString(descriptor->package_id, packageId) ||
        !descriptorString(descriptor->package_version, packageVersion) ||
        !descriptorString(descriptor->target_abi, targetAbi) ||
        !descriptorString(descriptor->compiler_identity, compilerIdentity) ||
        packageId != proof.packageId || packageVersion != proof.packageVersion ||
        targetAbi != proof.targetAbi || compilerIdentity != proof.compilerIdentity) {
        error = "Native v2 library descriptor identity does not match its proof";
        return false;
    }
    if (descriptor->export_count > v1->export_count ||
        descriptor->export_count > MaxNativeExportCount ||
        (descriptor->export_count != 0 && !descriptor->exports)) {
        error = "Native v2 library descriptor has an invalid export table";
        return false;
    }
    std::set<std::string> symbols;
    std::set<std::string> linkages;
    std::unordered_map<std::string, const LunaNativeExportDescriptorV1*>
        oldRows;
    oldRows.reserve(static_cast<size_t>(v1->export_count));
    for (uint64_t index = 0; index < v1->export_count; ++index)
        oldRows.emplace(v1->exports[index].symbol_id, &v1->exports[index]);
    std::vector<std::string> canonicalRows;
    canonicalRows.reserve(static_cast<size_t>(descriptor->export_count));
    for (uint64_t index = 0; index < descriptor->export_count; ++index) {
        const auto& row = descriptor->exports[index];
        std::string symbol, contract, linkage;
        if (row.abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V2 ||
            row.struct_size != sizeof(LunaNativeExportDescriptorV2) ||
            row.declaration_kind != LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
            row.flags != LUNA_NATIVE_EXPORT_CALLABLE_V1 ||
            row.entry_abi != LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1 ||
            row.reserved_zero != 0 || !row.entry ||
            !descriptorString(row.symbol_id, symbol) ||
            !descriptorString(row.contract_id, contract) ||
            !descriptorString(row.linkage_name, linkage) ||
            !symbols.insert(symbol).second ||
            !linkages.insert(linkage).second) {
            error = "Native v2 library descriptor contains an invalid export row";
            return false;
        }
        const auto old = oldRows.find(symbol);
        if (old == oldRows.end() ||
            contract != old->second->contract_id ||
            linkage != old->second->linkage_name ||
            row.declaration_kind != old->second->declaration_kind ||
            row.flags != old->second->flags ||
            row.entry != old->second->entry) {
            error = "Native v2 export row differs from its verified v1 row";
            return false;
        }
        canonicalRows.push_back(canonicalNativeTypedExport(
            row.declaration_kind, row.flags, row.entry_abi,
            symbol, contract, linkage));
    }
    const auto digest = digestNativeTypedExports(std::move(canonicalRows));
    if (!std::equal(digest.begin(), digest.end(),
                    descriptor->export_descriptor_digest)) {
        error = "Native v2 export rows do not match their descriptor digest";
        return false;
    }
    return true;
}

bool validateNativeDescriptorV3(
    void* handle, const std::string& stagedPath, const NativeProofInfo& proof,
    const LunaNativeLibraryDescriptorV1* v1,
    const LunaNativeLibraryDescriptorV2* v2,
    const LunaNativeLibraryDescriptorV3*& descriptor, std::string& error) {
    auto* rawQuery = loadNativeSymbol(
        handle, "luna_native_library_descriptor_v3");
    if (!rawQuery) return true;
    if (!nativeAddressInStagedImage(handle, stagedPath, rawQuery)) {
        error = "Native v3 descriptor query is outside its verified image";
        return false;
    }
    const auto query = reinterpret_cast<LunaNativeLibraryDescriptorFnV3>(rawQuery);
    descriptor = query();
    if (!descriptor || descriptor->magic != LUNA_NATIVE_DESCRIPTOR_MAGIC_V3 ||
        descriptor->abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V3 ||
        descriptor->struct_size != sizeof(LunaNativeLibraryDescriptorV3) ||
        descriptor->reserved_zero != 0) {
        error = "verified Native image returned an invalid v3 library descriptor";
        return false;
    }
    std::string packageId, packageVersion, targetAbi, compilerIdentity;
    if (!descriptorString(descriptor->package_id, packageId) ||
        !descriptorString(descriptor->package_version, packageVersion) ||
        !descriptorString(descriptor->target_abi, targetAbi) ||
        !descriptorString(descriptor->compiler_identity, compilerIdentity) ||
        packageId != proof.packageId || packageVersion != proof.packageVersion ||
        targetAbi != proof.targetAbi || compilerIdentity != proof.compilerIdentity) {
        error = "Native v3 library descriptor identity does not match its proof";
        return false;
    }
    if (descriptor->export_count > MaxNativeExportCount ||
        (descriptor->export_count != 0 && !descriptor->exports)) {
        error = "Native v3 library descriptor has an invalid export table";
        return false;
    }
    std::set<std::string> v1Symbols, v1Linkages;
    std::set<const void*> v1Entries;
    for (uint64_t index = 0; index < v1->export_count; ++index) {
        const auto& old = v1->exports[index];
        v1Symbols.insert(old.symbol_id);
        if ((old.flags & LUNA_NATIVE_EXPORT_CALLABLE_V1) != 0) {
            v1Linkages.insert(old.linkage_name);
            v1Entries.insert(old.entry);
        }
    }
    std::set<std::string> v2Symbols;
    if (v2)
        for (uint64_t index = 0; index < v2->export_count; ++index)
            v2Symbols.insert(v2->exports[index].symbol_id);
    std::set<std::string> symbols, linkages;
    std::vector<std::string> canonicalRows;
    canonicalRows.reserve(static_cast<size_t>(descriptor->export_count));
    for (uint64_t index = 0; index < descriptor->export_count; ++index) {
        const auto& row = descriptor->exports[index];
        const char* rawIdentifiers[] = {
            row.symbol_id, row.contract_id, row.linkage_name,
            row.ref_slot_symbol_id, row.ref_slot_contract_id,
            row.result_type_id, row.error_type_id, row.error_abi_layout_id,
            row.error_drop_symbol_id, row.error_drop_contract_id};
        std::array<std::string, 10> identifiers;
        bool validIdentifiers = true;
        for (size_t field = 0; field < identifiers.size(); ++field)
            validIdentifiers &= descriptorString(
                rawIdentifiers[field], identifiers[field]);
        if (row.abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V3 ||
            row.struct_size != sizeof(LunaNativeExportDescriptorV3) ||
            row.declaration_kind != LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
            row.flags != LUNA_NATIVE_EXPORT_CALLABLE_V1 ||
            row.entry_abi != LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1 ||
            row.ref_mode != LUNA_NATIVE_REF_SHARED_BORROW_V1 ||
            row.result_mode != LUNA_NATIVE_RESULT_I32_OWNED_ERROR_V1 ||
            row.status_domain != LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1 ||
            row.error_value_size == 0 || row.error_value_size > (1u << 20) ||
            row.error_value_alignment == 0 ||
            row.error_value_alignment > 4096 ||
            (row.error_value_alignment & (row.error_value_alignment - 1)) != 0 ||
            !row.entry || !validIdentifiers ||
            !symbols.insert(identifiers[0]).second ||
            !linkages.insert(identifiers[2]).second ||
            v1Symbols.count(identifiers[0]) != 0 ||
            v1Linkages.count(identifiers[2]) != 0 ||
            v1Entries.count(row.entry) != 0 ||
            v2Symbols.count(identifiers[0]) != 0) {
            error = "Native v3 library descriptor contains an invalid export row";
            return false;
        }
        if (row.entry != loadNativeSymbol(handle, identifiers[2].c_str())) {
            error = "Native v3 entry does not match its resolved symbol";
            return false;
        }
        if (!nativeAddressInStagedImage(handle, stagedPath, row.entry)) {
            error = "Native v3 entry is outside its verified image";
            return false;
        }
        canonicalRows.push_back(canonicalNativeOwnedResultExportV3(
            row.declaration_kind, row.flags, row.entry_abi, row.ref_mode,
            row.result_mode, row.status_domain, row.error_value_size,
            row.error_value_alignment, identifiers));
    }
    const auto digest = digestNativeTypedExports(std::move(canonicalRows));
    if (!std::equal(digest.begin(), digest.end(),
                    descriptor->export_descriptor_digest)) {
        error = "Native v3 export rows do not match their descriptor digest";
        return false;
    }
    return true;
}

bool validateNativeDescriptorV4(
    void* handle, const std::string& stagedPath, const NativeProofInfo& proof,
    const LunaNativeLibraryDescriptorV1* v1,
    const LunaNativeLibraryDescriptorV2* v2,
    const LunaNativeLibraryDescriptorV3* v3,
    const LunaNativeLibraryDescriptorV4*& descriptor,
    std::vector<std::string>& proofExports, std::string& error) {
    auto* rawQuery = loadNativeSymbol(
        handle, "luna_native_library_descriptor_v4");
    if (!rawQuery) return true;
    if (!nativeAddressInStagedImage(handle, stagedPath, rawQuery)) {
        error = "Native v4 descriptor query is outside its verified image";
        return false;
    }
    const auto query = reinterpret_cast<LunaNativeLibraryDescriptorFnV4>(rawQuery);
    descriptor = query();
    if (descriptor &&
        !nativeAddressInStagedImage(handle, stagedPath, descriptor)) {
        error = "Native v4 descriptor is outside its verified image";
        return false;
    }
    if (!descriptor || descriptor->magic != LUNA_NATIVE_DESCRIPTOR_MAGIC_V4 ||
        descriptor->abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V4 ||
        descriptor->struct_size != sizeof(LunaNativeLibraryDescriptorV4) ||
        descriptor->reserved_zero != 0) {
        error = "verified Native image returned an invalid v4 library descriptor";
        return false;
    }
    std::string packageId, packageVersion, targetAbi, compilerIdentity;
    if (!descriptorString(descriptor->package_id, packageId) ||
        !descriptorString(descriptor->package_version, packageVersion) ||
        !descriptorString(descriptor->target_abi, targetAbi) ||
        !descriptorString(descriptor->compiler_identity, compilerIdentity) ||
        packageId != proof.packageId || packageVersion != proof.packageVersion ||
        targetAbi != proof.targetAbi || compilerIdentity != proof.compilerIdentity) {
        error = "Native v4 library descriptor identity does not match its proof";
        return false;
    }
    if (descriptor->export_count > MaxNativeExportCount ||
        (descriptor->export_count != 0 && !descriptor->exports)) {
        error = "Native v4 library descriptor has an invalid export table";
        return false;
    }
    if (descriptor->exports &&
        !nativeAddressInStagedImage(handle, stagedPath,
                                    descriptor->exports)) {
        error = "Native v4 export table is outside its verified image";
        return false;
    }
    std::set<std::string> priorSymbols, priorLinkages;
    std::set<const void*> priorEntries;
    std::set<std::pair<std::string, std::string>> proofBoundSlots;
    for (uint64_t index = 0; index < v1->export_count; ++index) {
        const auto& row = v1->exports[index];
        priorSymbols.insert(row.symbol_id);
        if (row.declaration_kind == LUNA_NATIVE_DECLARATION_SLOT_V1)
            proofBoundSlots.emplace(row.symbol_id, row.contract_id);
        if ((row.flags & LUNA_NATIVE_EXPORT_CALLABLE_V1) != 0) {
            priorLinkages.insert(row.linkage_name);
            priorEntries.insert(row.entry);
        }
    }
    if (v2)
        for (uint64_t index = 0; index < v2->export_count; ++index)
            priorSymbols.insert(v2->exports[index].symbol_id);
    if (v3)
        for (uint64_t index = 0; index < v3->export_count; ++index) {
            const auto& row = v3->exports[index];
            priorSymbols.insert(row.symbol_id);
            priorLinkages.insert(row.linkage_name);
            priorEntries.insert(row.entry);
        }
    std::set<std::string> symbols, linkages;
    std::set<const void*> entries;
    std::vector<std::string> canonicalRows;
    canonicalRows.reserve(static_cast<size_t>(descriptor->export_count));
    for (uint64_t index = 0; index < descriptor->export_count; ++index) {
        const auto& row = descriptor->exports[index];
        const char* rawIdentifiers[] = {
            row.symbol_id, row.contract_id, row.linkage_name,
            row.module_name, row.package_id, row.function_type_id,
            row.ref_type_id, row.ref_slot_symbol_id,
            row.ref_slot_contract_id, row.ref_slot_type_id};
        std::array<std::string, 10> identifiers;
        bool validIdentifiers = true;
        for (size_t field = 0; field < identifiers.size(); ++field)
            validIdentifiers &= descriptorString(
                rawIdentifiers[field], identifiers[field]);
        if (row.abi_version != LUNA_NATIVE_DESCRIPTOR_ABI_V4 ||
            row.struct_size != sizeof(LunaNativeExportDescriptorV4) ||
            row.declaration_kind != LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
            row.flags != LUNA_NATIVE_EXPORT_CALLABLE_V1 ||
            row.entry_abi != LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1 ||
            row.ref_mode != LUNA_NATIVE_REF_SHARED_BORROW_V1 ||
            row.result_mode != LUNA_NATIVE_RESULT_UNIT_V1 ||
            row.status_domain != LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1 ||
            !row.entry || !validIdentifiers ||
            identifiers[3] != proof.packageId ||
            identifiers[4] != proof.packageId ||
            !symbols.insert(identifiers[0]).second ||
            !linkages.insert(identifiers[2]).second ||
            !entries.insert(row.entry).second ||
            priorSymbols.count(identifiers[0]) != 0 ||
            priorLinkages.count(identifiers[2]) != 0 ||
            priorEntries.count(row.entry) != 0) {
            error = "Native v4 library descriptor contains an invalid export row";
            return false;
        }
        if (row.entry != loadNativeSymbol(handle, identifiers[2].c_str())) {
            error = "Native v4 entry does not match its resolved symbol";
            return false;
        }
        if (!nativeAddressInStagedImage(handle, stagedPath, row.entry)) {
            error = "Native v4 entry is outside its verified image";
            return false;
        }
        canonicalRows.push_back(canonicalNativeRefUnitExportV4(
            row.declaration_kind, row.flags, row.entry_abi, row.ref_mode,
            row.result_mode, row.status_domain, identifiers));
    }
    const auto digest = digestNativeTypedExports(canonicalRows);
    if (!std::equal(digest.begin(), digest.end(),
                    descriptor->export_descriptor_digest)) {
        error = "Native v4 export rows do not match their descriptor digest";
        return false;
    }
    for (uint64_t index = 0; index < descriptor->export_count; ++index) {
        const auto& row = descriptor->exports[index];
        if (proofBoundSlots.count(
                {row.ref_slot_symbol_id, row.ref_slot_contract_id}) == 0) {
            error = "Native v4 Ref target has no proof-bound public Slot";
            return false;
        }
    }
    proofExports.insert(proofExports.end(),
                        canonicalRows.begin(), canonicalRows.end());
    return true;
}


} // namespace

VerifiedNativeLibrary::~VerifiedNativeLibrary() {
    reset();
}

VerifiedNativeLibrary::VerifiedNativeLibrary(
    VerifiedNativeLibrary&& other) noexcept {
    *this = std::move(other);
}

VerifiedNativeLibrary& VerifiedNativeLibrary::operator=(
    VerifiedNativeLibrary&& other) noexcept {
    if (this == &other) return *this;
    reset();
    nativeHandle_ = std::exchange(other.nativeHandle_, nullptr);
    stagingHandle_ = std::exchange(other.stagingHandle_, -1);
    stagedPath_ = std::move(other.stagedPath_);
    stagedDirectory_ = std::move(other.stagedDirectory_);
    descriptor_ = std::exchange(other.descriptor_, nullptr);
    descriptorV2_ = std::exchange(other.descriptorV2_, nullptr);
    descriptorV3_ = std::exchange(other.descriptorV3_, nullptr);
    descriptorV4_ = std::exchange(other.descriptorV4_, nullptr);
    typedExportsBySymbol_ = std::move(other.typedExportsBySymbol_);
    other.typedExportsBySymbol_.clear();
    proof_ = std::move(other.proof_);
    return *this;
}

void VerifiedNativeLibrary::reset() noexcept {
    closeNativeImage(nativeHandle_);
    nativeHandle_ = nullptr;
    descriptor_ = nullptr;
    descriptorV2_ = nullptr;
    descriptorV3_ = nullptr;
    descriptorV4_ = nullptr;
    typedExportsBySymbol_.clear();
    StagedNativeImage staged;
    staged.handle = std::exchange(stagingHandle_, -1);
    staged.path = std::move(stagedPath_);
    staged.directory = std::move(stagedDirectory_);
    releaseStagedImage(staged);
    proof_ = {};
}

const LunaNativeExportDescriptorV1* VerifiedNativeLibrary::findExport(
    const std::string& symbolId, const std::string& contractId) const {
    if (!descriptor_) return nullptr;
    for (uint64_t index = 0; index < descriptor_->export_count; ++index) {
        const auto& exported = descriptor_->exports[index];
        if (symbolId == exported.symbol_id && contractId == exported.contract_id)
            return &exported;
    }
    return nullptr;
}

bool VerifiedNativeLibrary::callI32NoArgs(
    const std::string& symbolId, const std::string& contractId,
    int32_t& result, std::string& error) const {
    if (!descriptorV2_) {
        error = "Native library has no v2 typed descriptor";
        return false;
    }
    const auto found = typedExportsBySymbol_.find(symbolId);
    if (found == typedExportsBySymbol_.end() ||
        contractId != found->second->contract_id ||
        found->second->entry_abi !=
            LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1) {
        error = "Native export has no C i32() entry profile";
        return false;
    }
    const auto* row = found->second;
    using Entry = int32_t (*)();
    static_assert(sizeof(Entry) == sizeof(row->entry),
                  "host cannot represent a Native function pointer");
    Entry function = nullptr;
    std::memcpy(&function, &row->entry, sizeof(function));
    result = function();
    return true;
}

uint32_t VerifiedNativeLibrary::entryAbiForExport(
    const std::string& symbolId, const std::string& contractId) const {
    const auto found = typedExportsBySymbol_.find(symbolId);
    return found != typedExportsBySymbol_.end() &&
        contractId == found->second->contract_id
        ? found->second->entry_abi : 0;
}

bool VerifiedNativeLibrary::hasRefResultOwnerCandidate(
    const std::string& symbolId, const std::string& contractId) const {
    if (!descriptorV3_) return false;
    for (uint64_t index = 0; index < descriptorV3_->export_count; ++index) {
        const auto& row = descriptorV3_->exports[index];
        if (symbolId == row.symbol_id && contractId == row.contract_id &&
            row.entry_abi == LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1 &&
            row.status_domain == LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1)
            return true;
    }
    return false;
}

bool VerifiedNativeLibrary::hasRefUnitCandidate(
    const std::string& symbolId, const std::string& contractId) const {
    if (!descriptorV4_) return false;
    for (uint64_t index = 0; index < descriptorV4_->export_count; ++index) {
        const auto& row = descriptorV4_->exports[index];
        if (symbolId == row.symbol_id && contractId == row.contract_id &&
            row.entry_abi == LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1 &&
            row.status_domain == LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1)
            return true;
    }
    return false;
}

uint64_t VerifiedNativeLibrary::refUnitCandidateCount() const {
    return descriptorV4_ ? descriptorV4_->export_count : 0;
}

bool VerifiedNativeLibrary::refUnitCandidateAt(
    uint64_t index,
    luna::runtime::GenerationRefUnitCandidate& candidate) const {
    if (!descriptorV4_ || index >= descriptorV4_->export_count) return false;
    const auto& row = descriptorV4_->exports[index];
    candidate = {
        row.symbol_id, row.contract_id, row.linkage_name,
        row.module_name, row.package_id, row.function_type_id,
        row.ref_type_id, row.ref_slot_symbol_id, row.ref_slot_contract_id,
        row.ref_slot_type_id, row.entry_abi, row.ref_mode,
        row.result_mode, row.status_domain};
    return true;
}

bool VerifiedNativeLibrary::callRefUnit(
    const std::string& symbolId, const std::string& contractId,
    const luna::runtime::RuntimeFragmentExecutionContext& parent,
    const luna::runtime::RuntimeFragmentRefHandle& reference,
    int32_t& status, std::string& error) const {
    error.clear();
    if (!descriptorV4_) {
        error = "Native library has no v4 Ref/unit descriptor";
        return false;
    }
    const LunaNativeExportDescriptorV4* selected = nullptr;
    for (uint64_t index = 0; index < descriptorV4_->export_count; ++index) {
        const auto& row = descriptorV4_->exports[index];
        if (symbolId == row.symbol_id && contractId == row.contract_id) {
            selected = &row;
            break;
        }
    }
    if (!selected ||
        selected->entry_abi != LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1 ||
        selected->ref_mode != LUNA_NATIVE_REF_SHARED_BORROW_V1 ||
        selected->result_mode != LUNA_NATIVE_RESULT_UNIT_V1 ||
        selected->status_domain != LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1) {
        error = "Native export has no Ref/unit entry profile";
        return false;
    }
    const auto pinnedParent = parent;
    if (!pinnedParent ||
        luna_compiler_fragment_context_check(pinnedParent.opaque()) !=
            LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS) {
        error = "Native Ref/unit parent context is not live";
        return false;
    }
    luna::runtime::RuntimeFragmentRefHandle pinnedReference;
    if (!luna::runtime::pinRuntimeFragmentRefHandle(
            reference, pinnedReference, error))
        return false;
    if (luna_runtime_fragment_ref_check_v1(
            pinnedReference.opaque(), selected->ref_slot_symbol_id,
            selected->ref_slot_contract_id) !=
            LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) {
        error = "Native Ref/unit reference does not match its declared Slot";
        return false;
    }
    using Entry = LunaNativeRefUnitEntryV1;
    static_assert(sizeof(Entry) == sizeof(selected->entry),
                  "host cannot represent a Native Ref/unit function pointer");
    Entry function = nullptr;
    std::memcpy(&function, &selected->entry, sizeof(function));
    const int32_t returned =
        function(pinnedParent.opaque(), pinnedReference.opaque());
    if (returned < LUNA_NATIVE_REF_UNIT_SUCCESS_V1 ||
        returned > LUNA_NATIVE_REF_UNIT_EXECUTION_FAILURE_V1) {
        error = "Native Ref/unit entry returned an unknown status";
        return false;
    }
    status = returned;
    return true;
}

bool callVerifiedNativeRefUnit(
    const std::shared_ptr<const VerifiedNativeLibrary>& library,
    const std::string& symbolId, const std::string& contractId,
    const luna::runtime::RuntimeFragmentExecutionContext& parent,
    const luna::runtime::RuntimeFragmentRefHandle& reference,
    int32_t& status, std::string& error) {
    const auto pinnedLibrary = library;
    if (!pinnedLibrary || !*pinnedLibrary) {
        error = "Native Ref/unit call has no loaded verified library";
        return false;
    }
    return pinnedLibrary->callRefUnit(
        symbolId, contractId, parent, reference, status, error);
}

uint64_t VerifiedNativeLibrary::exportCount() const {
    return descriptor_ ? descriptor_->export_count : 0;
}

const LunaNativeExportDescriptorV1* VerifiedNativeLibrary::exportAt(
    uint64_t index) const {
    return descriptor_ && index < descriptor_->export_count
        ? &descriptor_->exports[index] : nullptr;
}

bool loadVerifiedNativeLibrary(
    const std::string& artifactPath, const std::string& trustStorePath,
    VerifiedNativeLibrary& library, std::string& error,
    void (*afterVerification)(void*), void* afterVerificationContext) {
    std::vector<uint8_t> sourceBytes;
    if (!readFile(artifactPath, sourceBytes, error)) return false;
    StagedNativeImage staged;
    if (!stageNativeImage(sourceBytes, staged, error)) return false;

    NativeProofInfo proof;
    if (!verifyNativeArtifact(staged.path, trustStorePath, proof, error)) {
        releaseStagedImage(staged);
        return false;
    }
    if (afterVerification) {
        try {
            afterVerification(afterVerificationContext);
        } catch (...) {
            error = "Native loader after-verification hook failed";
            releaseStagedImage(staged);
            return false;
        }
    }
    void* handle = openNativeImage(staged.path, error);
    if (!handle) {
        releaseStagedImage(staged);
        return false;
    }
    const LunaNativeLibraryDescriptorV1* descriptor = nullptr;
    const LunaNativeLibraryDescriptorV2* descriptorV2 = nullptr;
    const LunaNativeLibraryDescriptorV3* descriptorV3 = nullptr;
    const LunaNativeLibraryDescriptorV4* descriptorV4 = nullptr;
    std::vector<std::string> proofExports;
    if (!validateNativeDescriptor(handle, proof, descriptor,
                                  proofExports, error) ||
        !validateNativeDescriptorV2(handle, proof, descriptor,
                                    descriptorV2, error) ||
        !validateNativeDescriptorV3(handle, staged.path, proof, descriptor,
                                    descriptorV2, descriptorV3, error) ||
        !validateNativeDescriptorV4(handle, staged.path, proof, descriptor,
                                    descriptorV2, descriptorV3,
                                    descriptorV4, proofExports, error)) {
        closeNativeImage(handle);
        releaseStagedImage(staged);
        return false;
    }
    // V4 candidates share the proof claim with the v1 public Slot registry.
    // Older libraries without v4 retain their original v1-only digest.
    if (digestList(std::move(proofExports)) != proof.exportDigest) {
        error = "Native library descriptor does not match its proof export digest";
        closeNativeImage(handle);
        releaseStagedImage(staged);
        return false;
    }

    VerifiedNativeLibrary loaded;
    loaded.nativeHandle_ = handle;
    loaded.stagingHandle_ = staged.handle;
    loaded.stagedPath_ = std::move(staged.path);
    loaded.stagedDirectory_ = std::move(staged.directory);
    loaded.descriptor_ = descriptor;
    loaded.descriptorV2_ = descriptorV2;
    loaded.descriptorV3_ = descriptorV3;
    loaded.descriptorV4_ = descriptorV4;
    loaded.proof_ = std::move(proof);
    staged.handle = -1;
    if (descriptorV2) {
        loaded.typedExportsBySymbol_.reserve(
            static_cast<size_t>(descriptorV2->export_count));
        for (uint64_t index = 0; index < descriptorV2->export_count; ++index)
            loaded.typedExportsBySymbol_.emplace(
                descriptorV2->exports[index].symbol_id,
                &descriptorV2->exports[index]);
    }
    library = std::move(loaded);
    return true;
}


} // namespace luna::driver
