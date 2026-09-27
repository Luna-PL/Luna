#pragma once

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#endif

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace luna::benchmarks::affinity {
inline void require(bool condition, const char* message,
                    const std::string& error = {}) {
    if (!condition)
        throw std::runtime_error(message + (error.empty() ? "" : ": " + error));
}

struct AffinityInfo {
    std::size_t cpuLimit = 0;
    std::vector<std::size_t> allowedCpus;
    std::string processorGroup = "none";
    std::string unsupportedReason;
};

#if defined(_WIN32) || defined(__linux__)
inline void requireAffinityCall(bool success, const char* message) {
    if (!success) {
#if defined(_WIN32)
        const auto code = GetLastError();
#else
        const auto code = errno;
#endif
        throw std::runtime_error(std::string(message) + ": " + std::to_string(code));
    }
}
#endif

// Probe-only controls: never alter the host process or any Runtime API.
// Fixed-size masks fail closed on larger systems instead of guessing CPU IDs.
inline AffinityInfo queryAffinity() {
    AffinityInfo info;
#if defined(_WIN32)
    const auto groups = GetActiveProcessorGroupCount();
    requireAffinityCall(groups != 0, "could not query processor groups");
    if (groups != 1) {
        info.unsupportedReason = "windows_multiple_processor_groups";
        return info;
    }
    GROUP_AFFINITY threadMask{};
    DWORD_PTR processMask = 0, systemMask = 0;
    requireAffinityCall(GetThreadGroupAffinity(GetCurrentThread(), &threadMask) != 0,
                        "could not query thread affinity");
    require(threadMask.Group == 0, "unsupported Windows processor group");
    requireAffinityCall(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != 0,
                        "could not query process affinity");
    info.cpuLimit = sizeof(DWORD_PTR) * 8;
    info.processorGroup = "0";
    const auto allowed = threadMask.Mask & processMask & systemMask;
    for (std::size_t cpu = 0; cpu < info.cpuLimit; ++cpu)
        if ((allowed & (DWORD_PTR{1} << cpu)) != 0)
            info.allowedCpus.push_back(cpu);
#elif defined(__linux__)
    cpu_set_t mask{};
    requireAffinityCall(sched_getaffinity(0, sizeof(mask), &mask) == 0,
                        "could not query thread affinity (fixed-size mask)");
    info.cpuLimit = CPU_SETSIZE;
    for (std::size_t cpu = 0; cpu < info.cpuLimit; ++cpu)
        if (CPU_ISSET(static_cast<int>(cpu), &mask))
            info.allowedCpus.push_back(cpu);
#else
    info.unsupportedReason = "unsupported_platform";
    return info;
#endif
    require(!info.allowedCpus.empty(), "thread has no allowed CPU");
    return info;
}

inline int reportAffinity() {
    const auto info = queryAffinity();
    std::cout << "# protocol=luna.fragment-cost.affinity-info.v1\n";
    if (!info.unsupportedReason.empty()) {
        std::cout << "# supported=no\n# reason=" << info.unsupportedReason << '\n';
        return 0;
    }
    std::cout << "# supported=yes\n# cpu_limit=" << info.cpuLimit
              << "\n# processor_group=" << info.processorGroup << "\n# allowed_cpus=";
    for (std::size_t index = 0; index < info.allowedCpus.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << info.allowedCpus[index];
    }
    std::cout << '\n';
    return 0;
}

inline std::size_t parseCpu(const char* argument) {
    const std::string value(argument);
    std::size_t cpu = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), cpu);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                cpu <= 1023 && value == std::to_string(cpu),
            "CPU must be a canonical unsigned integer in 0..1023");
    return cpu;
}

inline AffinityInfo requireAllowedCpu(std::size_t cpu) {
    auto info = queryAffinity();
    require(info.unsupportedReason.empty(), "thread affinity is unsupported",
            info.unsupportedReason);
    require(std::find(info.allowedCpus.begin(), info.allowedCpus.end(), cpu) !=
                info.allowedCpus.end(),
            "requested CPU is outside the thread's allowed set");
    return info;
}

class PinnedThread {
public:
    explicit PinnedThread(std::size_t cpu) : cpu_(cpu), info_(requireAllowedCpu(cpu)) {
#if defined(_WIN32)
        requireAffinityCall(SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu_) != 0,
                            "could not pin measurement thread");
#elif defined(__linux__)
        cpu_set_t mask{};
        CPU_ZERO(&mask);
        CPU_SET(static_cast<int>(cpu_), &mask);
        requireAffinityCall(sched_setaffinity(0, sizeof(mask), &mask) == 0,
                            "could not pin measurement thread");
#endif
        verify();
    }

    void verify() const {
#if defined(_WIN32)
        GROUP_AFFINITY mask{};
        requireAffinityCall(GetThreadGroupAffinity(GetCurrentThread(), &mask) != 0,
                            "could not read back thread affinity");
        PROCESSOR_NUMBER current{};
        GetCurrentProcessorNumberEx(&current);
        require(mask.Group == 0 && mask.Mask == (DWORD_PTR{1} << cpu_) &&
                    current.Group == 0 && current.Number == cpu_,
                "measurement thread affinity/current CPU changed");
#elif defined(__linux__)
        cpu_set_t mask{};
        requireAffinityCall(sched_getaffinity(0, sizeof(mask), &mask) == 0,
                            "could not read back thread affinity");
        require(CPU_COUNT(&mask) == 1 && CPU_ISSET(static_cast<int>(cpu_), &mask) &&
                    sched_getcpu() == static_cast<int>(cpu_),
                "measurement thread affinity/current CPU changed");
#else
        throw std::runtime_error("thread affinity is unsupported");
#endif
    }

    void report(const char* controlDigest) const {
        std::cout << "# affinity_control_sha256=" << controlDigest << '\n'
                  << "# affinity=measurement_thread,logical_cpu=" << cpu_
                  << ",processor_group=" << info_.processorGroup
                  << ",verified=sample_boundaries,power_policy=uncontrolled\n";
    }

private:
    std::size_t cpu_;
    AffinityInfo info_;
};

} // namespace luna::benchmarks::affinity
