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

bool descriptorString(const char* source, std::string& value) {
    if (!source) return false;
    size_t length = 0;
    while (length < MaxNativeDescriptorString && source[length] != '\0')
        ++length;
    if (length == 0 || length == MaxNativeDescriptorString) return false;
    value.assign(source, length);
    return value.find_first_of("\r\n\t") == std::string::npos;
}

bool validateNativeDescriptor(
    void* handle, const NativeProofInfo& proof,
    const LunaNativeLibraryDescriptorV1*& descriptor, std::string& error) {
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
    std::vector<std::string> canonicalExports;
    canonicalExports.reserve(static_cast<size_t>(descriptor->export_count));
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
        canonicalExports.push_back(canonicalNativeExport(spec));
    }
    if (digestList(std::move(canonicalExports)) != proof.exportDigest) {
        error = "Native library descriptor does not match its proof export digest";
        return false;
    }
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
    proof_ = std::move(other.proof_);
    return *this;
}

void VerifiedNativeLibrary::reset() noexcept {
    closeNativeImage(nativeHandle_);
    nativeHandle_ = nullptr;
    descriptor_ = nullptr;
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
    if (!validateNativeDescriptor(handle, proof, descriptor, error)) {
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
    loaded.proof_ = std::move(proof);
    staged.handle = -1;
    library = std::move(loaded);
    return true;
}


} // namespace luna::driver
