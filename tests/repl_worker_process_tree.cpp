#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(LUNA_REPL_PROCESS_TREE_LIBRARY)

#if defined(_WIN32)
#define LUNA_REPL_TEST_EXPORT __declspec(dllexport)
#else
#define LUNA_REPL_TEST_EXPORT
#endif

extern "C" LUNA_REPL_TEST_EXPORT int32_t repl_spawn_descendant() {
    const char* executable = std::getenv("LUNA_REPL_DESCENDANT_EXECUTABLE");
    if (!executable || *executable == '\0') return -1;
#if defined(_WIN32)
    std::istringstream workerCommandLine(GetCommandLineA());
    std::string argument;
    while (workerCommandLine >> argument) {
        // Only result and completion remain live while linked JIT code runs.
        if (argument != "--result-channel" && argument != "--complete") continue;
        if (!(workerCommandLine >> argument)) return -1;
        uint64_t handleValue = 0;
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(),
                                            handleValue);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
            handleValue > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max()))
            return -1;
        DWORD flags = 0;
        if (!GetHandleInformation(
                reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue)), &flags))
            return -1;
        if ((flags & HANDLE_FLAG_INHERIT) != 0) {
            const char* marker = std::getenv("LUNA_REPL_DESCENDANT_FD_LEAK_MARKER");
            if (marker && *marker != '\0') {
                std::ofstream leakOutput(marker, std::ios::binary | std::ios::trunc);
                leakOutput << "worker left internal handle " << handleValue << " inheritable\n";
            }
            return -1;
        }
    }
    std::string commandLine = "\"" + std::string(executable) + "\"";
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(executable, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return -1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 4242;
#else
#if defined(__linux__)
    std::ifstream commandLine("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> arguments;
    std::string argument;
    while (std::getline(commandLine, argument, '\0'))
        arguments.push_back(argument);
    std::string internalDescriptors;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string& option = arguments[index];
        if (option != "--result-channel" && option != "--complete") continue;
        if (!internalDescriptors.empty()) internalDescriptors += ',';
        internalDescriptors += arguments[++index];
    }
    if (!internalDescriptors.empty())
        ::setenv("LUNA_REPL_INTERNAL_DESCRIPTORS", internalDescriptors.c_str(), 1);
#endif
    const pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        ::execl(executable, executable, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return 4242;
#endif
}

extern "C" LUNA_REPL_TEST_EXPORT int32_t repl_exceed_memory_limit() {
    constexpr size_t AllocationBytes = 384ULL * 1024 * 1024;
    auto* allocation = static_cast<unsigned char*>(std::malloc(AllocationBytes));
    if (!allocation) std::abort();
    for (size_t offset = 0; offset < AllocationBytes; offset += 4096)
        *reinterpret_cast<volatile unsigned char*>(allocation + offset) = 1;
    std::free(allocation);
    return 0;
}

extern "C" LUNA_REPL_TEST_EXPORT int32_t repl_process_local_sequence() {
    static int32_t sequence = 0;
    return ++sequence;
}

void repl_slow_process_exit() { std::this_thread::sleep_for(std::chrono::seconds(10)); }

extern "C" LUNA_REPL_TEST_EXPORT int32_t repl_register_slow_process_exit() {
    return std::atexit(repl_slow_process_exit) == 0 ? 31 : -1;
}

#elif defined(LUNA_REPL_DELAYED_INPUT)

int main() {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "= 23\n:quit\n" << std::flush;
    return 0;
}

#else

int main() {
    const char* marker = std::getenv("LUNA_REPL_DESCENDANT_MARKER");
    if (!marker || *marker == '\0') return 2;
    const char* descriptorLeakMarker = std::getenv("LUNA_REPL_DESCENDANT_FD_LEAK_MARKER");
    if (!descriptorLeakMarker || *descriptorLeakMarker == '\0') return 4;
#if defined(_WIN32)
    // Windows handle values are process-local and can be reused by the child.
    // The worker checks the real result/completion handles immediately before
    // CreateProcess instead of treating a numeric collision here as a leak.
    (void)descriptorLeakMarker;
#else
    const char* internalDescriptors = std::getenv("LUNA_REPL_INTERNAL_DESCRIPTORS");
    if (internalDescriptors) {
        std::istringstream descriptors(internalDescriptors);
        std::string descriptorText;
        while (std::getline(descriptors, descriptorText, ',')) {
            int descriptor = -1;
            const auto parsed = std::from_chars(
                descriptorText.data(), descriptorText.data() + descriptorText.size(), descriptor);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != descriptorText.data() + descriptorText.size())
                return 5;
            errno = 0;
            if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) {
                std::ofstream leakOutput(descriptorLeakMarker, std::ios::binary | std::ios::trunc);
                leakOutput << "descendant inherited internal descriptor " << descriptor << '\n';
                break;
            }
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    output << "descendant survived worker cleanup\n";
    return output ? 0 : 3;
}

#endif
