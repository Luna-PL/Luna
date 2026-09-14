#include "driver/ReplProcess.h"

#include "driver/ReplTransport.h"

#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <limits>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>
#endif

namespace luna::driver::repl_detail {
namespace {

bool prepareWorkerProcessGroup(std::ostream& errors) {
#if !defined(_WIN32)
    if (::setpgid(0, 0) != 0 && ::getpgrp() != ::getpid()) {
        errors << "error[repl-worker]: cannot establish process group: errno " << errno << '\n';
        return false;
    }
#else
    (void)errors;
#endif
    return true;
}

bool protectWorkerHandle(uint64_t token, llvm::StringRef name, std::ostream& errors) {
#if defined(_WIN32)
    if (token > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
        errors << "error[repl-worker]: invalid " << name.str() << " handle\n";
        return false;
    }
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token)),
                              HANDLE_FLAG_INHERIT, 0)) {
        errors << "error[repl-worker]: cannot protect " << name.str()
               << " handle from descendant inheritance: error " << GetLastError() << '\n';
        return false;
    }
#else
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        errors << "error[repl-worker]: invalid " << name.str() << " descriptor\n";
        return false;
    }
    const int descriptor = static_cast<int>(token);
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        errors << "error[repl-worker]: cannot protect " << name.str()
               << " descriptor from descendant inheritance: errno " << errno << '\n';
        return false;
    }
#endif
    return true;
}

bool redirectWorkerStream(uint64_t token, int targetDescriptor,
#if defined(_WIN32)
                          DWORD standardHandle,
#endif
                          llvm::StringRef name, std::ostream& errors) {
#if defined(_WIN32)
    if (token > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) return false;
    const intptr_t channelHandle = static_cast<intptr_t>(token);
    const int channelDescriptor = ::_open_osfhandle(channelHandle, _O_WRONLY | _O_BINARY);
    if (channelDescriptor < 0) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token)));
        errors << "error[repl-worker]: cannot bind " << name.str() << " channel to CRT\n";
        return false;
    }
    if (::_dup2(channelDescriptor, targetDescriptor) != 0) {
        ::_close(channelDescriptor);
        errors << "error[repl-worker]: cannot redirect " << name.str() << " descriptor\n";
        return false;
    }
    ::_close(channelDescriptor);
    const intptr_t redirectedHandle = ::_get_osfhandle(targetDescriptor);
    if (redirectedHandle == -1 ||
        !SetStdHandle(standardHandle, reinterpret_cast<HANDLE>(redirectedHandle))) {
        errors << "error[repl-worker]: cannot publish redirected " << name.str()
               << " handle: error " << GetLastError() << '\n';
        return false;
    }
#else
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    const int channel = static_cast<int>(token);
    if (::dup2(channel, targetDescriptor) < 0) {
        errors << "error[repl-worker]: cannot redirect " << name.str() << ": errno " << errno
               << '\n';
        ::close(channel);
        return false;
    }
    if (channel != targetDescriptor) ::close(channel);
#endif
    return true;
}

} // namespace

ProcessTreeGuard::ProcessTreeGuard(const llvm::sys::ProcessInfo& process, uint64_t memoryLimitBytes)
    : mProcess(process), mMemoryLimitBytes(memoryLimitBytes) {}

ProcessTreeGuard::~ProcessTreeGuard() {
#if defined(_WIN32)
    if (mJob) CloseHandle(mJob);
#else
    if (mAttached) ::kill(-static_cast<pid_t>(mProcess.Pid), SIGKILL);
#endif
}

bool ProcessTreeGuard::attach(std::string& error) {
    (void)error;
    (void)mMemoryLimitBytes;
#if defined(_WIN32)
    mJob = CreateJobObjectW(nullptr, nullptr);
    if (!mJob) {
        error = "CreateJobObject failed with error " + std::to_string(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (mMemoryLimitBytes != 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = static_cast<SIZE_T>(mMemoryLimitBytes);
    }
    if (!SetInformationJobObject(mJob, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        error = "SetInformationJobObject failed with error " + std::to_string(GetLastError());
        return false;
    }
    if (!AssignProcessToJobObject(mJob, static_cast<HANDLE>(mProcess.Process))) {
        error = "AssignProcessToJobObject failed with error " + std::to_string(GetLastError());
        return false;
    }
#endif
    mAttached = true;
    return true;
}

void ProcessTreeGuard::terminate() {
#if defined(_WIN32)
    if (mJob) {
        auto* job = static_cast<HANDLE>(mJob);
        // This is a private job configured with KILL_ON_JOB_CLOSE. Closing it
        // after the explicit termination also guarantees cleanup if
        // TerminateJobObject races process teardown or otherwise fails.
        TerminateJobObject(job, 1);
        CloseHandle(job);
        mJob = nullptr;
    } else if (mProcess.Process) {
        TerminateProcess(static_cast<HANDLE>(mProcess.Process), 1);
    }
#else
    if (mAttached)
        ::kill(-static_cast<pid_t>(mProcess.Pid), SIGKILL);
    else if (mProcess.Pid != llvm::sys::ProcessInfo::InvalidPid)
        ::kill(static_cast<pid_t>(mProcess.Pid), SIGKILL);
#endif
}

bool applyWorkerMemoryLimit(unsigned memoryLimitMiB, std::ostream& errors) {
#if defined(_WIN32)
    (void)memoryLimitMiB;
    (void)errors;
#else
    if (memoryLimitMiB == 0) return true;
    const uint64_t requestedBytes = static_cast<uint64_t>(memoryLimitMiB) * 1024 * 1024;
    if (requestedBytes > static_cast<uint64_t>(std::numeric_limits<rlim_t>::max())) {
        errors << "error[repl-worker]: memory limit does not fit platform rlimit\n";
        return false;
    }
    rlimit limit{};
    if (::getrlimit(RLIMIT_AS, &limit) != 0) {
        errors << "error[repl-worker]: cannot read address-space limit: errno " << errno << '\n';
        return false;
    }
    const rlim_t requested = static_cast<rlim_t>(requestedBytes);
    rlim_t effectiveLimit = requested;
    if (limit.rlim_cur != RLIM_INFINITY) effectiveLimit = std::min(effectiveLimit, limit.rlim_cur);
    if (limit.rlim_max != RLIM_INFINITY) effectiveLimit = std::min(effectiveLimit, limit.rlim_max);
    limit.rlim_cur = effectiveLimit;
    limit.rlim_max = effectiveLimit;
    if (::setrlimit(RLIMIT_AS, &limit) != 0) {
        errors << "error[repl-worker]: cannot apply address-space limit: errno " << errno << '\n';
        return false;
    }
#endif
    return true;
}

bool protectWorkerHandles(uint64_t requestChannel, uint64_t resultChannel, uint64_t stdoutChannel,
                          uint64_t stderrChannel, uint64_t readySignal, uint64_t gateSignal,
                          uint64_t completionSignal, std::ostream& errors) {
    const bool requestProtected = protectWorkerHandle(requestChannel, "request channel", errors);
    const bool resultProtected = protectWorkerHandle(resultChannel, "result channel", errors);
    const bool stdoutProtected = protectWorkerHandle(stdoutChannel, "stdout channel", errors);
    const bool stderrProtected = protectWorkerHandle(stderrChannel, "stderr channel", errors);
    const bool readyProtected = protectWorkerHandle(readySignal, "readiness signal", errors);
    const bool gateProtected = protectWorkerHandle(gateSignal, "gate signal", errors);
    const bool completionProtected =
        protectWorkerHandle(completionSignal, "completion signal", errors);
    return requestProtected && resultProtected && stdoutProtected && stderrProtected &&
           readyProtected && gateProtected && completionProtected;
}

bool redirectWorkerOutput(uint64_t stdoutChannel, uint64_t stderrChannel, std::ostream& errors) {
    if (!redirectWorkerStream(stderrChannel, 2,
#if defined(_WIN32)
                              STD_ERROR_HANDLE,
#endif
                              "stderr", errors))
        return false;
    return redirectWorkerStream(stdoutChannel, 1,
#if defined(_WIN32)
                                STD_OUTPUT_HANDLE,
#endif
                                "stdout", errors);
}

void closeWorkerOutput() {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
#if defined(_WIN32)
    ::_close(1);
    ::_close(2);
    SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
#else
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
#endif
}

bool waitForWorkerGate(uint64_t readySignal, uint64_t gateSignal, unsigned parentProcessId,
                       std::ostream& errors) {
    if (!prepareWorkerProcessGroup(errors)) return false;
#if defined(__linux__)
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        errors << "error[repl-worker]: cannot arm parent-death cleanup: errno " << errno << '\n';
        return false;
    }
    if (::getppid() != static_cast<pid_t>(parentProcessId)) {
        errors << "error[repl-worker]: parent exited during worker startup\n";
        return false;
    }
#endif
    if (!publishWorkerSignal(readySignal, "readiness", errors)) return false;
#if defined(_WIN32)
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentProcessId));
    if (!parent) {
        errors << "error[repl-worker]: cannot monitor parent process\n";
        return false;
    }
    HANDLE gate = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(gateSignal));
    const std::array<HANDLE, 2> handles = {gate, parent};
    const DWORD waitResult =
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
    CloseHandle(gate);
    CloseHandle(parent);
    if (waitResult != WAIT_OBJECT_0) {
        errors << "error[repl-worker]: parent exited or gate wait failed before execution\n";
        return false;
    }
#else
    if (gateSignal > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        errors << "error[repl-worker]: invalid gate signal descriptor\n";
        return false;
    }
    const int gate = static_cast<int>(gateSignal);
    char value = 0;
    ssize_t count = 0;
    do {
        count = ::recv(gate, &value, 1, 0);
    } while (count < 0 && errno == EINTR);
    ::close(gate);
    if (count != 1) {
        errors << "error[repl-worker]: parent exited or gate read failed before execution\n";
        return false;
    }
#endif
    return true;
}

} // namespace luna::driver::repl_detail
