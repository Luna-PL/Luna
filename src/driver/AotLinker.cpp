#include "driver/AotLinker.h"

#include "diagnostics/Diagnostic.h"

#include <llvm/Support/Process.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <llvm/TargetParser/Host.h>
#ifdef _WIN32
#include <llvm/Support/ConvertUTF.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef LUNA_DL_LIBRARY
#define LUNA_DL_LIBRARY ""
#endif

namespace luna::driver {
namespace {

void printErrors(const std::vector<diagnostic::Diagnostic>& errors) {
    for (const auto& error : errors) std::cerr << error << "\n";
}

std::string quoteForDisplay(const std::string& value) {
    if (value.find_first_of(" \t\"'") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

bool isLibraryPath(const std::string& value) {
    auto endsWith = [&value](const char* suffix) {
        const std::string suffixString(suffix);
        return value.size() >= suffixString.size() &&
               value.compare(
                   value.size() - suffixString.size(),
                   suffixString.size(),
                   suffixString) == 0;
    };
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos ||
           endsWith(".a") || endsWith(".so") || endsWith(".dylib") ||
           endsWith(".dll") || endsWith(".lib");
}

bool filesEqual(const std::filesystem::path& left,
                const std::filesystem::path& right) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_regular_file(left, error) || error) return false;
    if (!fs::is_regular_file(right, error) || error) return false;
    const auto leftSize = fs::file_size(left, error);
    if (error) return false;
    const auto rightSize = fs::file_size(right, error);
    if (error || leftSize != rightSize) return false;

    std::ifstream leftInput(left, std::ios::binary);
    std::ifstream rightInput(right, std::ios::binary);
    if (!leftInput || !rightInput) return false;
    std::array<char, 64 * 1024> leftBuffer{};
    std::array<char, 64 * 1024> rightBuffer{};
    while (leftInput && rightInput) {
        leftInput.read(leftBuffer.data(), leftBuffer.size());
        rightInput.read(rightBuffer.data(), rightBuffer.size());
        const auto leftCount = leftInput.gcount();
        const auto rightCount = rightInput.gcount();
        if (leftCount != rightCount ||
            !std::equal(leftBuffer.begin(), leftBuffer.begin() + leftCount,
                        rightBuffer.begin()))
            return false;
    }
    return true;
}

bool readFile(const std::filesystem::path& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    contents = stream.str();
    return input.eof() || input.good();
}

bool commitPendingFile(const std::filesystem::path& pending,
                       const std::filesystem::path& destination,
                       std::error_code& error) {
    namespace fs = std::filesystem;
    error.clear();
    const bool destinationExists = fs::exists(destination, error);
    if (error) return false;
    if (!destinationExists) {
        // Both paths are siblings. A rename avoids a second write and a
        // second antivirus/indexer scan on the common cold-build path.
        fs::rename(pending, destination, error);
        if (!error) return true;
        // A concurrent creator or platform-specific rename restriction may
        // still permit the established overwrite path.
        error.clear();
    }
    fs::copy_file(pending, destination, fs::copy_options::overwrite_existing,
                  error);
    std::error_code removeError;
    fs::remove(pending, removeError);
    return !error;
}

bool outputCoversDependency(const std::filesystem::path& output,
                            const std::filesystem::path& dependency) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto outputTime = fs::last_write_time(output, error);
    if (error) return false;
    const auto dependencyTime = fs::last_write_time(dependency, error);
    return !error && outputTime >= dependencyTime;
}

std::string linkState(const std::vector<std::string>& linkerArgs) {
    std::ostringstream state;
    state << "luna-aot-link-v1\n";
    for (const auto& argument : linkerArgs)
        state << argument.size() << ':' << argument << '\n';
    return state.str();
}

bool ignoredInputDirectory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == ".cache" || name == "build" ||
           name.rfind("build-", 0) == 0;
}

std::filesystem::path inputCachePath(const AotBuildCacheOptions& options) {
    return std::filesystem::path(options.inputPath) / "build" / "native" /
           ".luna-build-input-state";
}

void hashText(llvm::SHA256& hash, const std::string& value) {
    hash.update(llvm::StringRef(std::to_string(value.size())));
    hash.update(llvm::StringRef(":"));
    hash.update(llvm::StringRef(value));
}

bool hashFile(llvm::SHA256& hash, const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    hashText(hash, path.lexically_normal().generic_string());
    hashText(hash, std::to_string(size));
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0)
            hash.update(llvm::StringRef(buffer.data(), static_cast<size_t>(count)));
    }
    return input.eof();
}

std::string fileDigest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    llvm::SHA256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0)
            hash.update(llvm::StringRef(buffer.data(), static_cast<size_t>(count)));
    }
    if (!input.eof()) return {};
    const auto digest = hash.final();
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

bool hashFileMetadata(llvm::SHA256& hash, const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    if (error || !fs::is_regular_file(absolute, error) || error) return false;
    const auto size = fs::file_size(absolute, error);
    if (error) return false;
    const auto timestamp = fs::last_write_time(absolute, error);
    if (error) return false;
    hashText(hash, absolute.generic_string());
    hashText(hash, std::to_string(size));
    hashText(hash, std::to_string(
        static_cast<long long>(timestamp.time_since_epoch().count())));
    return true;
}

std::string effectiveRuntimeLibrary(const AotBuildCacheOptions& options) {
    if (!options.runtimeLibrary.empty()) return options.runtimeLibrary;
    if (const char* configured = std::getenv("LUNA_RUNTIME_LIB")) return configured;
    return std::string(BUILD_DIR) + "/libruntime.a";
}

std::string effectiveCompiler(const AotBuildCacheOptions& options) {
    if (!options.compiler.empty()) return options.compiler;
    if (const char* configured = std::getenv("LUNA_CXX")) return configured;
    return "clang++";
}

#ifdef _WIN32
struct MingwLldToolchain {
    std::filesystem::path linker;
    std::filesystem::path libraryDirectory;
    std::filesystem::path resourceLibraryDirectory;
    std::filesystem::path crt2;
    std::filesystem::path crtBegin;
    std::filesystem::path crtEnd;
    std::filesystem::path builtins;
    std::vector<std::filesystem::path> trackedFiles;
};

constexpr std::array<const char*, 5> clangDriverEnvironmentVariables = {
    "LIBRARY_PATH",
    "COMPILER_PATH",
    "GCC_EXEC_PREFIX",
    "CCC_OVERRIDE_OPTIONS",
    "CLANG_CONFIG_FILE",
};

bool hasClangDriverEnvironmentOverride() {
    for (const char* variable : clangDriverEnvironmentVariables) {
        const char* value = std::getenv(variable);
        if (value && *value) return true;
    }
    return false;
}

std::optional<MingwLldToolchain> findCompatibleMingwLld(
    const std::filesystem::path& compilerPath) {
#if !defined(__x86_64__) && !defined(_M_X64)
    (void)compilerPath;
    return std::nullopt;
#else
    namespace fs = std::filesystem;
    std::error_code error;
    if (hasClangDriverEnvironmentOverride()) return std::nullopt;

    std::string compilerName = compilerPath.stem().string();
    std::transform(compilerName.begin(), compilerName.end(),
                   compilerName.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (compilerName != "clang++" && compilerName.rfind("clang++-", 0) != 0)
        return std::nullopt;

    fs::path driverConfiguration = compilerPath;
    driverConfiguration.replace_extension(".cfg");
    if (fs::is_regular_file(driverConfiguration, error) && !error)
        return std::nullopt;
    error.clear();

    const fs::path binaryDirectory = compilerPath.parent_path();
    const fs::path toolchainRoot = binaryDirectory.parent_path();
    const fs::path libraryDirectory = toolchainRoot / "lib";
    const fs::path linker = binaryDirectory / "ld.lld.exe";
    const fs::path crt2 = libraryDirectory / "crt2.o";
    const fs::path crtBegin = libraryDirectory / "crtbegin.o";
    const fs::path crtEnd = libraryDirectory / "crtend.o";

    // This recipe is for the flat MSYS2 clang64 layout. If target-specific
    // search directories exist, their preferred archives must be selected by
    // the driver rather than guessed here.
    const fs::path targetLibraryDirectory =
        toolchainRoot / "x86_64-w64-mingw32" / "lib";
    const fs::path mingwLibraryDirectory =
        toolchainRoot / "x86_64-w64-mingw32" / "mingw" / "lib";
    if (fs::exists(targetLibraryDirectory, error) || error)
        return std::nullopt;
    error.clear();
    if (fs::exists(mingwLibraryDirectory, error) || error)
        return std::nullopt;
    error.clear();

    auto dynamicOrStaticArchive = [&libraryDirectory](const char* name) {
        fs::path dynamic = libraryDirectory / (std::string(name) + ".dll.a");
        std::error_code candidateError;
        if (fs::is_regular_file(dynamic, candidateError) && !candidateError)
            return dynamic;
        return libraryDirectory / (std::string(name) + ".a");
    };
    const fs::path cxxLibrary = dynamicOrStaticArchive("libc++");
    const fs::path unwindLibrary = dynamicOrStaticArchive("libunwind");
    const std::vector<fs::path> requiredFiles = {
        linker,
        crt2,
        crtBegin,
        crtEnd,
        cxxLibrary,
        libraryDirectory / "libmingw32.a",
        unwindLibrary,
        libraryDirectory / "libmoldname.a",
        libraryDirectory / "libmingwex.a",
        libraryDirectory / "libmsvcrt.a",
        libraryDirectory / "libadvapi32.a",
        libraryDirectory / "libshell32.a",
        libraryDirectory / "libuser32.a",
    };
    for (const auto& file : requiredFiles) {
        if (!fs::is_regular_file(file, error) || error) return std::nullopt;
    }
    if (!fs::is_regular_file(libraryDirectory / "libkernel32.a", error) ||
        error)
        return std::nullopt;

    // A normal Clang installation contains one active resource directory.
    // Refuse to guess if side-by-side versions make the companion compiler's
    // builtin archive ambiguous.
    std::vector<fs::path> builtinsArchives;
    const fs::path clangResources = libraryDirectory / "clang";
    fs::directory_iterator iterator(clangResources, error);
    const fs::directory_iterator end;
    if (error) return std::nullopt;
    for (; iterator != end; iterator.increment(error)) {
        if (error) return std::nullopt;
        if (!iterator->is_directory(error) || error) continue;
        const fs::path candidate =
            iterator->path() / "lib" / "windows" /
            "libclang_rt.builtins-x86_64.a";
        if (fs::is_regular_file(candidate, error) && !error)
            builtinsArchives.push_back(candidate);
        error.clear();
    }
    if (builtinsArchives.size() != 1) return std::nullopt;

    MingwLldToolchain result;
    result.linker = linker;
    result.libraryDirectory = libraryDirectory;
    result.resourceLibraryDirectory = builtinsArchives.front().parent_path();
    result.crt2 = crt2;
    result.crtBegin = crtBegin;
    result.crtEnd = crtEnd;
    result.builtins = builtinsArchives.front();
    result.trackedFiles = requiredFiles;
    result.trackedFiles.push_back(libraryDirectory / "libkernel32.a");
    result.trackedFiles.push_back(result.builtins);
    return result;
#endif
}

class ScopedWindowsHandle {
public:
    ScopedWindowsHandle() = default;
    explicit ScopedWindowsHandle(HANDLE handle) : mHandle(handle) {}
    ~ScopedWindowsHandle() { reset(); }

    ScopedWindowsHandle(const ScopedWindowsHandle&) = delete;
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&) = delete;

    HANDLE get() const { return mHandle; }
    HANDLE release() {
        const HANDLE handle = mHandle;
        mHandle = nullptr;
        return handle;
    }
    void reset(HANDLE handle = nullptr) {
        if (mHandle && mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
        mHandle = handle;
    }

private:
    HANDLE mHandle = nullptr;
};

ScopedWindowsHandle inheritableStandardHandle(DWORD identifier,
                                               DWORD nullAccess,
                                               std::string& error) {
    HANDLE source = GetStdHandle(identifier);
    ScopedWindowsHandle nullHandle;
    if (!source || source == INVALID_HANDLE_VALUE) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        source = CreateFileW(L"NUL", nullAccess,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
        if (source == INVALID_HANDLE_VALUE) {
            error = "cannot open NUL for AOT linker: Windows error " +
                    std::to_string(GetLastError());
            return {};
        }
        nullHandle.reset(source);
    }

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        error = "cannot duplicate AOT linker standard handle: Windows error " +
                std::to_string(GetLastError());
        return {};
    }
    return ScopedWindowsHandle(duplicate);
}

struct StreamedLinkResult {
    int status = -1;
    bool processStarted = false;
    std::string artifactDigest;
    std::string error;
};

StreamedLinkResult executeLldToHashedFile(
    const std::string& linkerExecutable,
    std::vector<std::string> arguments,
    const std::filesystem::path& pendingArtifactPath) {
    StreamedLinkResult result;
    bool replacedOutput = false;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "-o") {
            arguments[index + 1] = "-";
            replacedOutput = true;
            break;
        }
    }
    if (!replacedOutput) {
        result.error = "AOT linker command has no output argument";
        return result;
    }

    std::vector<llvm::StringRef> argumentRefs;
    argumentRefs.reserve(arguments.size());
    for (const auto& argument : arguments) argumentRefs.emplace_back(argument);
    auto commandLine = llvm::sys::flattenWindowsCommandLine(argumentRefs);
    if (!commandLine) {
        result.error = "cannot encode AOT linker command: " +
                       commandLine.getError().message();
        return result;
    }
    std::wstring executableWide;
    if (!llvm::ConvertUTF8toWide(linkerExecutable, executableWide)) {
        result.error = "cannot encode AOT linker executable path";
        return result;
    }
    std::vector<wchar_t> mutableCommandLine(commandLine->begin(),
                                            commandLine->end());
    mutableCommandLine.push_back(L'\0');

    SECURITY_ATTRIBUTES pipeAttributes{};
    pipeAttributes.nLength = sizeof(pipeAttributes);
    pipeAttributes.bInheritHandle = TRUE;
    HANDLE rawReadHandle = nullptr;
    HANDLE rawWriteHandle = nullptr;
    if (!CreatePipe(&rawReadHandle, &rawWriteHandle, &pipeAttributes,
                    64 * 1024)) {
        result.error = "cannot create AOT linker output pipe: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }
    ScopedWindowsHandle readHandle(rawReadHandle);
    ScopedWindowsHandle writeHandle(rawWriteHandle);
    if (!SetHandleInformation(readHandle.get(), HANDLE_FLAG_INHERIT, 0)) {
        result.error = "cannot protect AOT linker output pipe: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }

    ScopedWindowsHandle childInput = inheritableStandardHandle(
        STD_INPUT_HANDLE, GENERIC_READ, result.error);
    if (!childInput.get()) return result;
    ScopedWindowsHandle childError = inheritableStandardHandle(
        STD_ERROR_HANDLE, GENERIC_WRITE, result.error);
    if (!childError.get()) return result;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput.get();
    startup.StartupInfo.hStdOutput = writeHandle.get();
    startup.StartupInfo.hStdError = childError.get();

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) {
        result.error = "cannot size AOT linker handle list: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }
    std::vector<unsigned char> attributeStorage(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attributeBytes)) {
        result.error = "cannot initialize AOT linker handle list: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }
    const std::array<HANDLE, 3> inheritedHandles = {
        childInput.get(), writeHandle.get(), childError.get()};
    if (!UpdateProcThreadAttribute(
            startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inheritedHandles.data()),
            sizeof(HANDLE) * inheritedHandles.size(), nullptr, nullptr)) {
        result.error = "cannot restrict AOT linker handles: Windows error " +
                       std::to_string(GetLastError());
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return result;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executableWide.c_str(), mutableCommandLine.data(), nullptr, nullptr,
        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        result.error = "cannot start AOT linker: Windows error " +
                       std::to_string(createError);
        return result;
    }
    result.processStarted = true;

    ScopedWindowsHandle processHandle(process.hProcess);
    ScopedWindowsHandle threadHandle(process.hThread);
    writeHandle.reset();
    childInput.reset();
    childError.reset();

    std::ofstream output(pendingArtifactPath,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        TerminateProcess(processHandle.get(), 1);
        WaitForSingleObject(processHandle.get(), INFINITE);
        result.error = "cannot create pending AOT artifact";
        return result;
    }

    llvm::SHA256 hash;
    std::array<char, 64 * 1024> buffer{};
    bool streamSucceeded = true;
    uint64_t totalBytes = 0;
    for (;;) {
        DWORD count = 0;
        if (ReadFile(readHandle.get(), buffer.data(),
                     static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            if (count == 0) break;
            output.write(buffer.data(), count);
            hash.update(llvm::StringRef(buffer.data(), count));
            totalBytes += count;
            if (!output) streamSucceeded = false;
            continue;
        }
        const DWORD readError = GetLastError();
        if (readError != ERROR_BROKEN_PIPE) {
            result.error = "cannot read AOT linker output: Windows error " +
                           std::to_string(readError);
            streamSucceeded = false;
        }
        break;
    }
    output.close();
    if (!output) streamSucceeded = false;

    if (WaitForSingleObject(processHandle.get(), INFINITE) != WAIT_OBJECT_0) {
        result.error = "cannot wait for AOT linker: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        result.error = "cannot read AOT linker status: Windows error " +
                       std::to_string(GetLastError());
        return result;
    }
    result.status = static_cast<int>(exitCode);
    if (result.status != 0 || !streamSucceeded) {
        if (!streamSucceeded) result.status = -1;
        if (result.error.empty() && !streamSucceeded)
            result.error = "cannot write pending AOT artifact";
        return result;
    }
    if (totalBytes == 0) {
        result.status = -1;
        result.error = "AOT linker produced an empty artifact";
        return result;
    }

    const auto digest = hash.final();
    std::ostringstream digestText;
    digestText << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        digestText << std::setw(2) << static_cast<unsigned>(byte);
    result.artifactDigest = digestText.str();
    return result;
}
#endif

std::string buildInputState(const AotBuildCacheOptions& options) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path packageRoot = fs::absolute(options.inputPath, error).lexically_normal();
    if (error || !fs::is_directory(packageRoot, error) || error) return {};

    fs::path scanRoot = packageRoot;
    for (fs::path current = packageRoot; !current.empty();) {
        if (fs::is_regular_file(current / "luna.workspace", error) && !error) {
            scanRoot = current;
            break;
        }
        error.clear();
        const fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }

    std::vector<fs::path> inputs;
    fs::recursive_directory_iterator iterator(
        scanRoot, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) return {};
    for (; iterator != end; iterator.increment(error)) {
        if (error) return {};
        if (iterator->is_directory(error)) {
            if (error) return {};
            if (ignoredInputDirectory(iterator->path())) iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            if (error) return {};
            continue;
        }
        const auto& path = iterator->path();
        const std::string name = path.filename().string();
        if (path.extension() == ".luna" || name == "luna.package" ||
            name == "luna.workspace" || name == "luna.lock")
            inputs.push_back(path.lexically_normal());
    }
    std::sort(inputs.begin(), inputs.end());
    if (inputs.empty()) return {};

    llvm::SHA256 hash;
    hashText(hash, "luna-aot-input-v3");
    hashText(hash, packageRoot.generic_string());
    hashText(hash, options.outputPath);
    hashText(hash, std::to_string(static_cast<int>(options.optimizationLevel)));
    hashText(hash, options.gpuTargets.emitPTX ? "ptx" : "no-ptx");
    hashText(hash, options.gpuTargets.cudaArchitecture);
    hashText(hash, options.gpuTargets.emitHSACO ? "hsaco" : "no-hsaco");
    hashText(hash, options.gpuTargets.rocmArchitecture);
    hashText(hash, options.reserveKernelRuntime ? "reserve-runtime" : "reachable-runtime");
    hashText(hash, llvm::sys::getProcessTriple());
    hashText(hash, llvm::sys::getHostCPUName().str());
    for (const auto& library : options.linkLibraries) {
        // Bare -l names cannot be mapped to a unique file without performing
        // the linker's full search. Keep those builds on the exact IR cache.
        if (!isLibraryPath(library)) return {};
        hashText(hash, library);
    }
    if (const char* path = std::getenv("PATH")) hashText(hash, path);
#ifdef _WIN32
    for (const char* variable : clangDriverEnvironmentVariables) {
        hashText(hash, variable);
        const char* value = std::getenv(variable);
        hashText(hash, value ? value : "");
    }
#endif
    for (const auto& input : inputs)
        if (!hashFile(hash, input)) return {};

    if (!hashFileMetadata(hash, options.compilerExecutable)) return {};
    const std::string runtimeLibrary = effectiveRuntimeLibrary(options);
    if (!hashFileMetadata(hash, runtimeLibrary)) return {};
    const std::string compiler = effectiveCompiler(options);
    auto compilerPath = llvm::sys::findProgramByName(compiler);
    if (!compilerPath || !hashFileMetadata(hash, *compilerPath)) return {};
#ifdef _WIN32
    if (const auto toolchain = findCompatibleMingwLld(*compilerPath)) {
        for (const auto& dependency : toolchain->trackedFiles)
            if (!hashFileMetadata(hash, dependency)) return {};
    }
#endif
    for (const auto& library : options.linkLibraries)
        if (isLibraryPath(library) && !hashFileMetadata(hash, library)) return {};

    const auto digest = hash.final();
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

void updateBuildCache(const AotLinkOptions& options,
                      const std::filesystem::path& artifactPath,
                      const std::filesystem::path& irPath,
                      const std::filesystem::path& objectPath,
                      const std::string& knownArtifactDigest = {}) {
    namespace fs = std::filesystem;
    if (!options.buildCache || options.buildCacheInputState.empty()) return;
    const std::string currentState = buildInputState(*options.buildCache);
    if (currentState.empty() || currentState != options.buildCacheInputState) return;

    const fs::path statePath = inputCachePath(*options.buildCache);
    std::error_code error;
    fs::create_directories(statePath.parent_path(), error);
    if (error) return;
    fs::path pendingPath = statePath;
    pendingPath += ".tmp." + std::to_string(llvm::sys::Process::getProcessId());
    const std::string artifactDigest = knownArtifactDigest.empty()
        ? fileDigest(artifactPath)
        : knownArtifactDigest;
    const std::string irDigest = fileDigest(irPath);
    const std::string objectDigest = fileDigest(objectPath);
    if (artifactDigest.empty() || irDigest.empty() || objectDigest.empty()) return;
    {
        std::ofstream output(pendingPath, std::ios::binary | std::ios::trunc);
        output << "luna-aot-input-v3\n" << currentState << '\n'
               << artifactPath.lexically_normal().generic_string() << '\n'
               << irPath.lexically_normal().generic_string() << '\n'
               << objectPath.lexically_normal().generic_string() << '\n'
               << artifactDigest << '\n' << irDigest << '\n'
               << objectDigest << '\n';
        if (!output) return;
    }
    commitPendingFile(pendingPath, statePath, error);
}

} // namespace

AotBuildCacheProbe AotLinker::probeBuildCache(const AotBuildCacheOptions& options) {
    namespace fs = std::filesystem;
    AotBuildCacheProbe result;
    result.inputState = buildInputState(options);
    if (result.inputState.empty()) return result;

    const fs::path statePath = inputCachePath(options);
    std::ifstream input(statePath, std::ios::binary);
    std::string marker, state, artifact, ir, object;
    std::string artifactDigest, irDigest, objectDigest;
    if (!std::getline(input, marker) || !std::getline(input, state) ||
        !std::getline(input, artifact) || !std::getline(input, ir) ||
        !std::getline(input, object) || !std::getline(input, artifactDigest) ||
        !std::getline(input, irDigest) || !std::getline(input, objectDigest) ||
        marker != "luna-aot-input-v3" || state != result.inputState)
        return result;

    std::error_code error;
    const bool pathsAndTimesCurrent =
        fs::is_regular_file(artifact, error) && !error &&
        fs::is_regular_file(ir, error) && !error &&
        fs::is_regular_file(object, error) && !error &&
        outputCoversDependency(statePath, artifact) &&
        outputCoversDependency(statePath, ir) &&
        outputCoversDependency(statePath, object) &&
        outputCoversDependency(artifact, object);
    const bool artifactMatches = pathsAndTimesCurrent &&
                                 fileDigest(artifact) == artifactDigest;
    const bool irMatches = pathsAndTimesCurrent && fileDigest(ir) == irDigest;
    const bool objectMatches = pathsAndTimesCurrent &&
                               fileDigest(object) == objectDigest;
    result.current = pathsAndTimesCurrent && artifactMatches && irMatches &&
                     objectMatches;
    // The legacy IR/link-state cache is timestamp based. If content changed
    // while timestamps still look current, ensure the fallback path repairs
    // the output instead of blessing the modified artifact with a new digest.
    result.forceRelink = pathsAndTimesCurrent && !artifactMatches;
    if (result.current) result.artifactPath = std::move(artifact);
    return result;
}

int AotLinker::build(CodeGenerator& codeGenerator, AotLinkOptions options) {
    namespace fs = std::filesystem;

    const fs::path inputPath(options.inputPath);
    fs::path irPath;
    fs::path artifactPath;
    if (fs::is_directory(inputPath)) {
        const std::string packageName = options.declaredPackageName.empty()
            ? inputPath.filename().string()
            : options.declaredPackageName;
        irPath = inputPath / (packageName + ".ll");
        artifactPath = inputPath / packageName;
    } else {
        irPath = inputPath.string() + ".ll";
        artifactPath = inputPath.parent_path() / inputPath.stem();
    }
    if (!options.outputPath.empty()) {
        artifactPath = fs::path(options.outputPath);
        irPath = artifactPath;
        irPath += ".ll";
    }
#ifdef _WIN32
    if (options.outputPath.empty() &&
        options.artifactKind == AotArtifactKind::Executable)
        artifactPath += ".exe";
#endif

    std::error_code filesystemError;
    if (!artifactPath.parent_path().empty())
        fs::create_directories(artifactPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
            "driver",
            "cannot create AOT output directory: " +
                filesystemError.message(),
            artifactPath.parent_path().string(), 0, 0,
            "check the output path permissions") << "\n";
        return 1;
    }

    fs::path pendingIrPath = irPath;
    pendingIrPath += ".tmp." +
                     std::to_string(llvm::sys::Process::getProcessId());
    fs::remove(pendingIrPath, filesystemError);
    filesystemError.clear();

    std::cout << "Emitting LLVM IR: " << irPath.string() << "\n";
    if (!codeGenerator.emitObjectFile(pendingIrPath.string())) {
        fs::remove(pendingIrPath, filesystemError);
        printErrors(codeGenerator.errors());
        return 1;
    }
    const bool irUnchanged = filesEqual(pendingIrPath, irPath);
    if (irUnchanged) {
        fs::remove(pendingIrPath, filesystemError);
    } else {
        if (!commitPendingFile(pendingIrPath, irPath, filesystemError)) {
            std::cerr << diagnostic::format(
                             "driver",
                             "cannot update AOT IR output: " +
                                 filesystemError.message(),
                             irPath.string(), 0, 0,
                             "check the output path permissions")
                      << "\n";
            return 1;
        }
    }

    // AOT remains self-contained when run from the build tree, but an
    // installed driver can supply its runtime and compiler explicitly or
    // through environment variables. This makes packaging reproducible
    // without silently linking against an unrelated build directory.
    if (options.runtimeLibrary.empty()) {
        if (const char* configured = std::getenv("LUNA_RUNTIME_LIB"))
            options.runtimeLibrary = configured;
    }
    if (options.runtimeLibrary.empty())
        options.runtimeLibrary = std::string(BUILD_DIR) + "/libruntime.a";
    if (!fs::exists(options.runtimeLibrary)) {
        std::cerr << diagnostic::format(
            "driver",
            "runtime library does not exist: '" + options.runtimeLibrary + "'",
            options.runtimeLibrary,
            0,
            0,
            "pass `--runtime-lib <path>` or set LUNA_RUNTIME_LIB to Luna's libruntime.a")
                  << "\n";
        return 1;
    }

    if (options.compiler.empty()) {
        if (const char* configured = std::getenv("LUNA_CXX"))
            options.compiler = configured;
    }
    if (options.compiler.empty()) options.compiler = "clang++";

    fs::path objectPath = artifactPath;
    objectPath += ".o";
    fs::path pendingObjectPath = objectPath;
    pendingObjectPath += ".tmp." +
                         std::to_string(llvm::sys::Process::getProcessId());
    fs::remove(pendingObjectPath, filesystemError);
    filesystemError.clear();
    std::cout << "Emitting native object: " << objectPath.string() << "\n";
    if (!codeGenerator.emitNativeObjectFile(pendingObjectPath.string())) {
        fs::remove(pendingObjectPath, filesystemError);
        printErrors(codeGenerator.errors());
        return 1;
    }
    const bool objectUnchanged = filesEqual(pendingObjectPath, objectPath);
    if (objectUnchanged) {
        fs::remove(pendingObjectPath, filesystemError);
    } else {
        if (!commitPendingFile(pendingObjectPath, objectPath, filesystemError)) {
            std::cerr << diagnostic::format(
                             "driver",
                             "cannot update AOT native object: " +
                                 filesystemError.message(),
                             objectPath.string(), 0, 0,
                             "check the output path permissions")
                      << "\n";
            return 1;
        }
    }

    const char* optimizationFlag = "-O0";
    if (options.optimizationLevel == LunaOptimizationLevel::O2)
        optimizationFlag = "-O2";
    else if (options.optimizationLevel == LunaOptimizationLevel::O3)
        optimizationFlag = "-O3";

    auto compilerPath = llvm::sys::findProgramByName(options.compiler);
    if (!compilerPath) {
        std::cerr << diagnostic::format(
            "driver",
            "cannot find AOT compiler '" + options.compiler + "': " +
                compilerPath.getError().message(),
            options.compiler,
            0,
            0,
            "pass --cc with an executable path or add the compiler to PATH")
                  << "\n";
        return 1;
    }

    std::vector<std::string> linkerArgs = {
        *compilerPath,
        optimizationFlag,
    };
    if (options.artifactKind == AotArtifactKind::SharedLibrary) {
#ifdef __APPLE__
        linkerArgs.push_back("-dynamiclib");
#else
        linkerArgs.push_back("-shared");
#endif
    }
#ifdef __APPLE__
    linkerArgs.push_back("-Wl,-dead_strip");
    linkerArgs.push_back("-Wl,-S");
#else
    // libruntime is built with per-symbol sections. Discard unused sections
    // here so small AOT programs only carry the runtime ABI they reference.
    linkerArgs.push_back("-Wl,--gc-sections");
    // Runtime archives may come from a RelWithDebInfo compiler build. Those
    // implementation-only debug sections otherwise leak into every user AOT
    // artifact even though Luna does not yet emit source-level debug metadata.
    linkerArgs.push_back("-Wl,--strip-debug");
#endif
    linkerArgs.push_back(objectPath.generic_string());
    linkerArgs.push_back(options.runtimeLibrary);
    if (std::string(LUNA_DL_LIBRARY).size())
        linkerArgs.push_back("-l" + std::string(LUNA_DL_LIBRARY));
    linkerArgs.push_back("-o");
    linkerArgs.push_back(artifactPath.generic_string());
    for (const auto& library : options.linkLibraries)
        linkerArgs.push_back(isLibraryPath(library) ? library : "-l" + library);

    std::string linkerExecutable = *compilerPath;
    std::vector<fs::path> directLinkDependencies;
#ifdef _WIN32
    bool streamDirectArtifact = false;
    // Clang's MinGW driver starts a second process after expanding a stable
    // CRT/library recipe. For the ordinary executable case, use the companion
    // ld.lld directly when the complete known layout is present. Any driver
    // customization, shared-library build, or searched user library keeps the
    // general Clang path above.
    const bool hasSearchedUserLibrary =
        std::any_of(options.linkLibraries.begin(), options.linkLibraries.end(),
                    [](const std::string& library) {
                        return !isLibraryPath(library);
                    });
    if (options.artifactKind == AotArtifactKind::Executable &&
        std::string(LUNA_DL_LIBRARY).empty() &&
        !hasSearchedUserLibrary) {
        if (const auto toolchain = findCompatibleMingwLld(*compilerPath)) {
            const fs::path toolchainRoot =
                toolchain->libraryDirectory.parent_path();
            linkerExecutable = toolchain->linker.generic_string();
            linkerArgs = {
                linkerExecutable,
                "-m",
                "i386pep",
                "-Bdynamic",
                "-o",
                artifactPath.generic_string(),
                toolchain->crt2.generic_string(),
                toolchain->crtBegin.generic_string(),
                "-L" + (toolchainRoot / "x86_64-w64-mingw32" / "lib").generic_string(),
                "-L" + (toolchainRoot / "x86_64-w64-mingw32" / "mingw" / "lib").generic_string(),
                "-L" + toolchain->libraryDirectory.generic_string(),
                "-L" + toolchain->resourceLibraryDirectory.generic_string(),
                "--gc-sections",
                "--strip-debug",
                objectPath.generic_string(),
                options.runtimeLibrary,
            };
            for (const auto& library : options.linkLibraries)
                linkerArgs.push_back(library);
            linkerArgs.insert(linkerArgs.end(), {
                "-lc++",
                "-lmingw32",
                toolchain->builtins.generic_string(),
                "-lunwind",
                "-lmoldname",
                "-lmingwex",
                "-lmsvcrt",
                "-ladvapi32",
                "-lshell32",
                "-luser32",
                "-lkernel32",
                "-lmingw32",
                toolchain->builtins.generic_string(),
                "-lunwind",
                "-lmoldname",
                "-lmingwex",
                "-lmsvcrt",
                "-lkernel32",
                toolchain->crtEnd.generic_string(),
            });
            directLinkDependencies = toolchain->trackedFiles;
            streamDirectArtifact = true;
        }
    }
#endif

    fs::path linkStatePath = artifactPath;
    linkStatePath += ".luna-link-state";
    const std::string expectedLinkState = linkState(linkerArgs);
    std::string existingLinkState;
    bool artifactIsCurrent =
        irUnchanged && objectUnchanged &&
        fs::is_regular_file(artifactPath, filesystemError) &&
        !filesystemError && readFile(linkStatePath, existingLinkState) &&
        existingLinkState == expectedLinkState &&
        outputCoversDependency(linkStatePath, artifactPath) &&
        outputCoversDependency(artifactPath, objectPath) &&
        outputCoversDependency(artifactPath, options.runtimeLibrary) &&
        outputCoversDependency(artifactPath, *compilerPath);
    for (const auto& dependency : directLinkDependencies) {
        if (artifactIsCurrent)
            artifactIsCurrent =
                outputCoversDependency(artifactPath, dependency);
    }
    if (options.buildCache && options.buildCache->forceRelink)
        artifactIsCurrent = false;
    for (const auto& library : options.linkLibraries) {
        if (artifactIsCurrent && isLibraryPath(library))
            artifactIsCurrent = outputCoversDependency(artifactPath, library);
    }
    if (artifactIsCurrent) {
        updateBuildCache(options, artifactPath, irPath, objectPath);
        std::cout << "Up to date: " << artifactPath.string() << "\n";
        return 0;
    }

    std::cout << "Linking:";
    for (const auto& argument : linkerArgs)
        std::cout << ' ' << quoteForDisplay(argument);
    std::cout << "\n";

    std::vector<llvm::StringRef> linkerArgRefs;
    linkerArgRefs.reserve(linkerArgs.size());
    for (const auto& argument : linkerArgs)
        linkerArgRefs.emplace_back(argument);

    // Once a relink starts, the previous state must not authorize reuse of a
    // partially replaced or externally produced artifact if clang fails.
    std::error_code invalidateStateError;
    fs::remove(linkStatePath, invalidateStateError);

    std::string executionError;
    std::string streamedArtifactDigest;
    int linkResult = 0;
#ifdef _WIN32
    fs::path pendingArtifactPath;
    if (streamDirectArtifact) {
        pendingArtifactPath = artifactPath;
        pendingArtifactPath += ".tmp.link." +
                               std::to_string(llvm::sys::Process::getProcessId());
        fs::remove(pendingArtifactPath, filesystemError);
        filesystemError.clear();
        auto streamed = executeLldToHashedFile(
            linkerExecutable, linkerArgs, pendingArtifactPath);
        if (!streamed.processStarted) {
            // Handle-list APIs or pipe creation can be unavailable in a
            // restricted Windows host. Preserve the established linker path
            // instead of making the optimization a compatibility requirement.
            linkResult = llvm::sys::ExecuteAndWait(
                linkerExecutable, linkerArgRefs, std::nullopt, {}, 0, 0,
                &executionError);
            if (linkResult != 0 && executionError.empty())
                executionError = std::move(streamed.error);
        } else {
            linkResult = streamed.status;
            executionError = std::move(streamed.error);
            streamedArtifactDigest = std::move(streamed.artifactDigest);
            if (linkResult == 0 &&
                !commitPendingFile(pendingArtifactPath, artifactPath,
                                   filesystemError)) {
                std::cerr << diagnostic::format(
                                 "driver",
                                 "cannot publish AOT artifact: " +
                                     filesystemError.message(),
                                 artifactPath.string(), 0, 0,
                                 "check the output path permissions")
                          << "\n";
                return 1;
            }
        }
    } else
#endif
    {
        linkResult = llvm::sys::ExecuteAndWait(
            linkerExecutable,
            linkerArgRefs,
            std::nullopt,
            {},
            0,
            0,
            &executionError);
    }
    if (linkResult != 0) {
#ifdef _WIN32
        if (!pendingArtifactPath.empty()) {
            std::error_code removeError;
            fs::remove(pendingArtifactPath, removeError);
        }
#endif
        std::cerr << diagnostic::format(
            "driver",
            "AOT linker '" + linkerExecutable + "' failed with status " +
                std::to_string(linkResult) +
                (executionError.empty() ? "" : ": " + executionError),
            "",
            0,
            0,
            "inspect the linker command above; verify --cc, --runtime-lib, and every --link dependency")
                  << "\n";
        return 1;
    }

    fs::path pendingLinkStatePath = linkStatePath;
    pendingLinkStatePath += ".tmp." +
                            std::to_string(llvm::sys::Process::getProcessId());
    {
        std::ofstream stateOutput(pendingLinkStatePath,
                                  std::ios::binary | std::ios::trunc);
        stateOutput << expectedLinkState;
    }
    filesystemError.clear();
    commitPendingFile(pendingLinkStatePath, linkStatePath, filesystemError);

    updateBuildCache(options, artifactPath, irPath, objectPath,
                     streamedArtifactDigest);

    std::cout << "Built "
              << (options.artifactKind == AotArtifactKind::SharedLibrary
                      ? "shared library: " : "executable: ")
              << artifactPath.string() << "\n";
    return 0;
}

} // namespace luna::driver
