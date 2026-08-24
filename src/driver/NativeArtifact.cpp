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

namespace luna::driver {
namespace {

using Digest = std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE>;
constexpr size_t MaxNativeArtifactBytes = 512u * 1024u * 1024u;
constexpr uint64_t MaxNativeExportCount = 1u << 20;
constexpr size_t MaxNativeDescriptorString = 4096;

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

Digest sha256(const std::vector<uint8_t>& bytes) {
    llvm::SHA256 hash;
    hash.update(llvm::ArrayRef<uint8_t>(bytes));
    return hash.final();
}

Digest digestList(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::vector<uint8_t> canonical;
    canonical.reserve(4 + values.size() * 8);
    canonical.resize(4);
    writeU32(canonical, 0, static_cast<uint32_t>(values.size()));
    for (const auto& value : values) {
        const size_t offset = canonical.size();
        canonical.resize(offset + 4 + value.size());
        writeU32(canonical, offset, static_cast<uint32_t>(value.size()));
        std::copy(value.begin(), value.end(), canonical.begin() + offset + 4);
    }
    return sha256(canonical);
}

template <class ELFT>
bool collectElfDependencies(
    const llvm::object::ELFObjectFile<ELFT>& object,
    std::vector<std::string>& dependencies, std::string& error) {
    const auto& elf = object.getELFFile();
    auto entries = elf.dynamicEntries();
    if (!entries) {
        error = "cannot read ELF dynamic table: " +
            llvm::toString(entries.takeError());
        return false;
    }
    uint64_t stringTableAddress = 0;
    uint64_t stringTableSize = 0;
    for (const auto& entry : *entries) {
        if (entry.d_tag == llvm::ELF::DT_STRTAB)
            stringTableAddress = entry.d_un.d_ptr;
        else if (entry.d_tag == llvm::ELF::DT_STRSZ)
            stringTableSize = entry.d_un.d_val;
    }
    if (stringTableAddress == 0 || stringTableSize == 0) {
        error = "ELF Native library has no bounded dynamic string table";
        return false;
    }
    auto stringTable = elf.toMappedAddr(stringTableAddress);
    if (!stringTable) {
        error = "cannot map ELF dynamic string table: " +
            llvm::toString(stringTable.takeError());
        return false;
    }
    for (const auto& entry : *entries) {
        if (entry.d_tag != llvm::ELF::DT_NEEDED) continue;
        const uint64_t offset = entry.d_un.d_val;
        if (offset >= stringTableSize) {
            error = "ELF DT_NEEDED offset exceeds its dynamic string table";
            return false;
        }
        const auto* name = reinterpret_cast<const char*>(*stringTable + offset);
        const size_t remaining = static_cast<size_t>(stringTableSize - offset);
        const void* terminator = std::memchr(name, 0, remaining);
        if (!terminator || name[0] == '\0') {
            error = "ELF DT_NEEDED contains an invalid library name";
            return false;
        }
        dependencies.emplace_back(
            name, static_cast<const char*>(terminator) - name);
    }
    return true;
}

bool collectPlatformDependencies(const std::string& artifactPath,
                                 std::vector<std::string>& dependencies,
                                 std::string& error) {
    auto binary = llvm::object::createBinary(artifactPath);
    if (!binary) {
        error = "cannot parse Native library format: " +
            llvm::toString(binary.takeError());
        return false;
    }
    llvm::object::Binary* image = binary->getBinary();
    if (auto* object = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(image))
        return collectElfDependencies(*object, dependencies, error);
    if (auto* object = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(image))
        return collectElfDependencies(*object, dependencies, error);
    if (auto* object = llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(image))
        return collectElfDependencies(*object, dependencies, error);
    if (auto* object = llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(image))
        return collectElfDependencies(*object, dependencies, error);
    if (auto* object = llvm::dyn_cast<llvm::object::COFFObjectFile>(image)) {
        for (const auto& directory : object->import_directories()) {
            llvm::StringRef name;
            if (llvm::Error entryError = directory.getName(name)) {
                error = "cannot read PE import directory: " +
                    llvm::toString(std::move(entryError));
                return false;
            }
            if (name.empty()) {
                error = "PE import directory contains an empty library name";
                return false;
            }
            dependencies.push_back(name.str());
        }
        for (const auto& directory : object->delay_import_directories()) {
            llvm::StringRef name;
            if (llvm::Error entryError = directory.getName(name)) {
                error = "cannot read PE delay-import directory: " +
                    llvm::toString(std::move(entryError));
                return false;
            }
            if (name.empty()) {
                error = "PE delay-import directory contains an empty library name";
                return false;
            }
            dependencies.push_back(name.str());
        }
        return true;
    }
    if (auto* object = llvm::dyn_cast<llvm::object::MachOObjectFile>(image)) {
        for (const auto& command : object->load_commands()) {
            switch (command.C.cmd) {
                case llvm::MachO::LC_LOAD_DYLIB:
                case llvm::MachO::LC_LOAD_WEAK_DYLIB:
                case llvm::MachO::LC_REEXPORT_DYLIB:
                case llvm::MachO::LC_LAZY_LOAD_DYLIB:
                case llvm::MachO::LC_LOAD_UPWARD_DYLIB:
                    break;
                default:
                    continue;
            }
            const auto dylib = object->getDylibIDLoadCommand(command);
            if (dylib.dylib.name >= dylib.cmdsize) {
                error = "Mach-O dylib command has an invalid name offset";
                return false;
            }
            const char* name = command.Ptr + dylib.dylib.name;
            const size_t remaining = dylib.cmdsize - dylib.dylib.name;
            const void* terminator = std::memchr(name, 0, remaining);
            if (!terminator || name[0] == '\0') {
                error = "Mach-O dylib command contains an invalid library name";
                return false;
            }
            dependencies.emplace_back(
                name, static_cast<const char*>(terminator) - name);
        }
        return true;
    }
    error = "unsupported Native library format for dependency proof";
    return false;
}

bool putString(std::vector<uint8_t>& record, size_t offset, size_t capacity,
               const std::string& value, const char* field,
               std::string& error) {
    if (value.empty() || value.size() >= capacity ||
        value.find_first_of("\r\n\t") != std::string::npos) {
        error = std::string("Native proof ") + field +
            " is empty, too long, or contains a control separator";
        return false;
    }
    std::copy(value.begin(), value.end(), record.begin() + offset);
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>& bytes,
              std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot read Native artifact '" + path + "'";
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) {
        error = "failed while reading Native artifact '" + path + "'";
        return false;
    }
    if (bytes.size() > MaxNativeArtifactBytes) {
        error = "Native artifact exceeds the 512 MiB loader limit";
        return false;
    }
    return true;
}

bool locateProof(const std::vector<uint8_t>& bytes, size_t& offset,
                 std::string& error) {
    const uint8_t magic[8] = {'L', 'U', 'N', 'A', 'N', 'P', '1', 0};
    size_t matches = 0;
    for (size_t index = 0; index + sizeof(LunaNativeProofV1) <= bytes.size();
         ++index) {
        if (std::memcmp(bytes.data() + index, magic, sizeof(magic)) != 0)
            continue;
        const uint8_t* candidate = bytes.data() + index;
        if (readU32(candidate + 8) != LUNA_NATIVE_PROOF_ABI_V1 ||
            readU32(candidate + 12) != sizeof(LunaNativeProofV1) ||
            readU32(candidate + 16) != LUNA_NATIVE_PROOF_DIGEST_SHA256 ||
            readU32(candidate + 20) != 0)
            continue;
        offset = index;
        ++matches;
    }
    if (matches != 1) {
        error = matches == 0
            ? "Native artifact has no valid embedded Luna proof"
            : "Native artifact contains multiple Luna proof records";
        return false;
    }
    return true;
}

bool decodeString(const uint8_t* bytes, size_t capacity,
                  std::string& value) {
    const void* terminator = std::memchr(bytes, 0, capacity);
    if (!terminator) return false;
    const auto* end = static_cast<const uint8_t*>(terminator);
    value.assign(reinterpret_cast<const char*>(bytes),
                 static_cast<size_t>(end - bytes));
    return !value.empty() && value.find_first_of("\r\n\t") == std::string::npos;
}

bool decodeProof(const std::vector<uint8_t>& bytes, size_t offset,
                 NativeProofInfo& info, std::string& error) {
    const uint8_t* proof = bytes.data() + offset;
    size_t cursor = 24;
    std::copy_n(proof + cursor, info.artifactDigest.size(),
                info.artifactDigest.begin());
    cursor += info.artifactDigest.size();
    std::copy_n(proof + cursor, info.exportDigest.size(),
                info.exportDigest.begin());
    cursor += info.exportDigest.size();
    std::copy_n(proof + cursor, info.dependencyDigest.size(),
                info.dependencyDigest.begin());
    cursor += info.dependencyDigest.size();
    if (!decodeString(proof + cursor, LUNA_NATIVE_PROOF_PACKAGE_ID_SIZE,
                      info.packageId)) {
        error = "Native proof contains an invalid package ID";
        return false;
    }
    cursor += LUNA_NATIVE_PROOF_PACKAGE_ID_SIZE;
    if (!decodeString(proof + cursor, LUNA_NATIVE_PROOF_PACKAGE_VERSION_SIZE,
                      info.packageVersion)) {
        error = "Native proof contains an invalid package version";
        return false;
    }
    cursor += LUNA_NATIVE_PROOF_PACKAGE_VERSION_SIZE;
    if (!decodeString(proof + cursor, LUNA_NATIVE_PROOF_TARGET_ABI_SIZE,
                      info.targetAbi)) {
        error = "Native proof contains an invalid target ABI";
        return false;
    }
    cursor += LUNA_NATIVE_PROOF_TARGET_ABI_SIZE;
    if (!decodeString(proof + cursor, LUNA_NATIVE_PROOF_COMPILER_ID_SIZE,
                      info.compilerIdentity)) {
        error = "Native proof contains an invalid compiler identity";
        return false;
    }
    return true;
}

Digest canonicalArtifactDigest(std::vector<uint8_t> bytes, size_t proofOffset) {
    std::fill_n(bytes.begin() + proofOffset, sizeof(LunaNativeProofV1), 0);
    return sha256(bytes);
}

bool parseAndDigest(const std::string& path, std::vector<uint8_t>& bytes,
                    size_t& proofOffset, NativeProofInfo& info,
                    Digest& actualDigest, std::string& error) {
    return readFile(path, bytes, error) &&
           locateProof(bytes, proofOffset, error) &&
           decodeProof(bytes, proofOffset, info, error) &&
           ((actualDigest = canonicalArtifactDigest(bytes, proofOffset)), true);
}

bool writeAllStagingBytes(int fd, const std::vector<uint8_t>& bytes,
                          std::string& error) {
#ifdef _WIN32
    (void)fd;
    (void)bytes;
    error = "internal Windows staging adapter mismatch";
    return false;
#else
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
#endif
}

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
            spec.declarationKind > LUNA_NATIVE_DECLARATION_METADATA_SCHEMA_V1 ||
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

bool makeNativeProofPlaceholder(const NativeProofSpec& spec,
                                std::vector<uint8_t>& record,
                                std::string& error) {
    record.assign(sizeof(LunaNativeProofV1), 0);
    const uint8_t magic[8] = {'L', 'U', 'N', 'A', 'N', 'P', '1', 0};
    std::copy(std::begin(magic), std::end(magic), record.begin());
    writeU32(record, 8, LUNA_NATIVE_PROOF_ABI_V1);
    writeU32(record, 12, sizeof(LunaNativeProofV1));
    writeU32(record, 16, LUNA_NATIVE_PROOF_DIGEST_SHA256);
    const Digest exportDigest = digestList(spec.exportedDescriptors);
    std::copy(exportDigest.begin(), exportDigest.end(), record.begin() + 56);
    return putString(record, 120, LUNA_NATIVE_PROOF_PACKAGE_ID_SIZE,
                     spec.packageId, "package ID", error) &&
           putString(record, 248, LUNA_NATIVE_PROOF_PACKAGE_VERSION_SIZE,
                     spec.packageVersion, "package version", error) &&
           putString(record, 280, LUNA_NATIVE_PROOF_TARGET_ABI_SIZE,
                     spec.targetAbi, "target ABI", error) &&
           putString(record, 408, LUNA_NATIVE_PROOF_COMPILER_ID_SIZE,
                     spec.compilerIdentity, "compiler identity", error);
}

bool sealNativeArtifact(const std::string& artifactPath,
                        const std::string& trustRecordPath,
                        NativeProofInfo& info, std::string& error) {
    std::vector<uint8_t> bytes;
    size_t proofOffset = 0;
    Digest actualDigest{};
    if (!parseAndDigest(artifactPath, bytes, proofOffset, info,
                        actualDigest, error))
        return false;
    const bool placeholder = std::all_of(
        info.artifactDigest.begin(), info.artifactDigest.end(),
        [](uint8_t byte) { return byte == 0; });
    if (!placeholder) {
        error = "Native artifact proof was already sealed";
        return false;
    }
    std::vector<std::string> dynamicDependencies;
    if (!collectPlatformDependencies(
            artifactPath, dynamicDependencies, error))
        return false;
    const Digest dependencyDigest = digestList(dynamicDependencies);
    std::fstream artifact(artifactPath,
                          std::ios::binary | std::ios::in | std::ios::out);
    artifact.seekp(static_cast<std::streamoff>(proofOffset + 88));
    artifact.write(reinterpret_cast<const char*>(dependencyDigest.data()),
                   static_cast<std::streamsize>(dependencyDigest.size()));
    artifact.seekp(static_cast<std::streamoff>(
        proofOffset + LUNA_NATIVE_PROOF_ARTIFACT_DIGEST_OFFSET));
    artifact.write(reinterpret_cast<const char*>(actualDigest.data()),
                   static_cast<std::streamsize>(actualDigest.size()));
    if (!artifact) {
        error = "cannot seal Native proof in '" + artifactPath + "'";
        return false;
    }
    artifact.close();
    info.artifactDigest = actualDigest;
    info.dependencyDigest = dependencyDigest;

    std::ofstream trust(trustRecordPath, std::ios::binary | std::ios::trunc);
    trust << nativeDigestHex(actualDigest) << '\t'
          << nativeDigestHex(info.exportDigest) << '\t'
          << nativeDigestHex(info.dependencyDigest) << '\t'
          << info.compilerIdentity << '\t' << info.packageId << '\t'
          << info.packageVersion << '\t' << info.targetAbi << '\n';
    if (!trust) {
        error = "cannot write Native trust candidate '" + trustRecordPath + "'";
        return false;
    }
    return true;
}

bool verifyNativeArtifact(const std::string& artifactPath,
                          const std::string& trustStorePath,
                          NativeProofInfo& info, std::string& error) {
    std::vector<uint8_t> bytes;
    size_t proofOffset = 0;
    Digest actualDigest{};
    if (!parseAndDigest(artifactPath, bytes, proofOffset, info,
                        actualDigest, error))
        return false;
    if (actualDigest != info.artifactDigest) {
        error = "Native artifact digest does not match its embedded proof";
        return false;
    }
    if (info.targetAbi != llvm::sys::getProcessTriple()) {
        error = "Native artifact target ABI '" + info.targetAbi +
            "' does not match host target '" +
            llvm::sys::getProcessTriple() + "'";
        return false;
    }
    std::vector<std::string> dynamicDependencies;
    if (!collectPlatformDependencies(
            artifactPath, dynamicDependencies, error))
        return false;
    if (digestList(dynamicDependencies) != info.dependencyDigest) {
        error = "Native proof does not match the final dynamic dependency table";
        return false;
    }
    std::ifstream trust(trustStorePath);
    if (!trust) {
        error = "cannot read explicit Native trust store '" + trustStorePath + "'";
        return false;
    }
    const std::string expected = nativeDigestHex(actualDigest) + '\t' +
        nativeDigestHex(info.exportDigest) + '\t' +
        nativeDigestHex(info.dependencyDigest) + '\t' +
        info.compilerIdentity + '\t' + info.packageId + '\t' +
        info.packageVersion + '\t' + info.targetAbi;
    std::string line;
    while (std::getline(trust, line)) {
        if (line == expected) return true;
    }
    error = "Native artifact is not present in the explicit trust store";
    return false;
}

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

std::string nativeDigestHex(const Digest& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const uint8_t byte : digest)
        output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

std::string canonicalNativeExport(const NativeExportSpec& descriptor) {
    std::ostringstream canonical;
    canonical << descriptor.declarationKind << '\n' << descriptor.flags << '\n'
              << descriptor.symbolId << '\n' << descriptor.contractId << '\n'
              << descriptor.linkageName;
    return canonical.str();
}

} // namespace luna::driver
