#include "driver/ReplTransport.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>

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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace luna::driver::repl_detail {

WorkerSignal::~WorkerSignal() {
#if defined(_WIN32)
    if (mHandle) CloseHandle(mHandle);
#else
    if (mParentFd >= 0) ::close(mParentFd);
    if (mChildFd >= 0) ::close(mChildFd);
#endif
}

bool WorkerSignal::create(bool childSignals, std::string& error) {
    mChildSignals = childSignals;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    mHandle = CreateEventW(&attributes, TRUE, FALSE, nullptr);
    if (!mHandle) {
        error = "CreateEvent failed with error " + std::to_string(GetLastError());
        return false;
    }
#else
    int descriptors[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
        error = "socketpair failed with errno " + std::to_string(errno);
        return false;
    }
    mParentFd = descriptors[0];
    mChildFd = descriptors[1];
#if defined(__APPLE__)
    const int suppressSigpipe = 1;
    if (::setsockopt(mParentFd, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigpipe,
                     sizeof(suppressSigpipe)) != 0 ||
        ::setsockopt(mChildFd, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigpipe,
                     sizeof(suppressSigpipe)) != 0) {
        error = "cannot suppress SIGPIPE on signal socket: errno " + std::to_string(errno);
        return false;
    }
#endif
    const int flags = ::fcntl(mParentFd, F_GETFD);
    if (flags < 0 || ::fcntl(mParentFd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        error = "cannot protect parent signal descriptor from inheritance: errno " +
                std::to_string(errno);
        return false;
    }
#endif
    return true;
}

uint64_t WorkerSignal::childToken() const {
#if defined(_WIN32)
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mHandle));
#else
    return static_cast<uint64_t>(mChildFd);
#endif
}

bool WorkerSignal::finishLaunch(std::string& error) {
#if defined(_WIN32)
    if (!SetHandleInformation(mHandle, HANDLE_FLAG_INHERIT, 0)) {
        error = "cannot clear signal inheritance with error " + std::to_string(GetLastError());
        return false;
    }
#else
    if (mChildFd >= 0) {
        ::close(mChildFd);
        mChildFd = -1;
    }
    (void)error;
#endif
    return true;
}

bool WorkerSignal::signal(std::string& error) {
    if (mChildSignals) {
        error = "cannot publish a parent signal on a child-to-parent channel";
        return false;
    }
#if defined(_WIN32)
    if (!SetEvent(mHandle)) {
        error = "SetEvent failed with error " + std::to_string(GetLastError());
        return false;
    }
#else
    const char value = 1;
    ssize_t written = 0;
    do {
        written = ::send(mParentFd, &value, 1,
#if defined(MSG_NOSIGNAL)
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
        error = "signal write failed with errno " + std::to_string(errno);
        return false;
    }
#endif
    return true;
}

SignalWaitResult WorkerSignal::wait(std::chrono::milliseconds timeout, std::string& error) {
    if (!mChildSignals) {
        error = "cannot wait for a child signal on a parent-to-child channel";
        return SignalWaitResult::Failed;
    }
#if defined(_WIN32)
    const DWORD result = WaitForSingleObject(mHandle, static_cast<DWORD>(timeout.count()));
    if (result == WAIT_OBJECT_0) return SignalWaitResult::Signaled;
    if (result == WAIT_TIMEOUT) return SignalWaitResult::TimedOut;
    error = "WaitForSingleObject failed with error " + std::to_string(GetLastError());
    return SignalWaitResult::Failed;
#else
    pollfd descriptor{};
    descriptor.fd = mParentFd;
    descriptor.events = POLLIN | POLLHUP;
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    if (result == 0) return SignalWaitResult::TimedOut;
    if (result < 0) {
        error = "poll failed with errno " + std::to_string(errno);
        return SignalWaitResult::Failed;
    }
    char value = 0;
    ssize_t count = 0;
    do {
        count = ::recv(mParentFd, &value, 1, 0);
    } while (count < 0 && errno == EINTR);
    if (count == 1) return SignalWaitResult::Signaled;
    if (count == 0) return SignalWaitResult::Closed;
    error = "signal read failed with errno " + std::to_string(errno);
    return SignalWaitResult::Failed;
#endif
}

WorkerDataChannel::~WorkerDataChannel() {
#if defined(_WIN32)
    if (mParentHandle) CloseHandle(mParentHandle);
    if (mChildHandle) CloseHandle(mChildHandle);
#else
    if (mParentFd >= 0) ::close(mParentFd);
    if (mChildFd >= 0) ::close(mChildFd);
#endif
}

bool WorkerDataChannel::create(WorkerChannelDirection direction, std::string& error) {
    mDirection = direction;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    constexpr DWORD PipeBufferBytes = 64 * 1024;
    if (!CreatePipe(&readHandle, &writeHandle, &attributes, PipeBufferBytes)) {
        error = "CreatePipe failed with error " + std::to_string(GetLastError());
        return false;
    }
    if (direction == WorkerChannelDirection::ParentToChild) {
        mParentHandle = writeHandle;
        mChildHandle = readHandle;
    } else {
        mParentHandle = readHandle;
        mChildHandle = writeHandle;
    }
    if (!SetHandleInformation(mParentHandle, HANDLE_FLAG_INHERIT, 0)) {
        error = "cannot protect parent channel handle from inheritance: error " +
                std::to_string(GetLastError());
        return false;
    }
#else
    int descriptors[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
        error = "socketpair failed with errno " + std::to_string(errno);
        return false;
    }
    mParentFd = descriptors[0];
    mChildFd = descriptors[1];
#if defined(__APPLE__)
    const int suppressSigpipe = 1;
    if (::setsockopt(mParentFd, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigpipe,
                     sizeof(suppressSigpipe)) != 0 ||
        ::setsockopt(mChildFd, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigpipe,
                     sizeof(suppressSigpipe)) != 0) {
        error = "cannot suppress SIGPIPE on worker channel: errno " + std::to_string(errno);
        return false;
    }
#endif
    const int descriptorFlags = ::fcntl(mParentFd, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl(mParentFd, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0) {
        error = "cannot protect parent channel descriptor from inheritance: errno " +
                std::to_string(errno);
        return false;
    }
    if (direction == WorkerChannelDirection::ChildToParent) {
        const int statusFlags = ::fcntl(mParentFd, F_GETFL);
        if (statusFlags < 0 || ::fcntl(mParentFd, F_SETFL, statusFlags | O_NONBLOCK) != 0) {
            error = "cannot make result channel nonblocking: errno " + std::to_string(errno);
            return false;
        }
    }
#endif
    return true;
}

uint64_t WorkerDataChannel::childToken() const {
#if defined(_WIN32)
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mChildHandle));
#else
    return static_cast<uint64_t>(mChildFd);
#endif
}

bool WorkerDataChannel::finishLaunch(std::string& error) {
#if defined(_WIN32)
    if (!mChildHandle) {
        error = "worker channel child handle is not available";
        return false;
    }
    CloseHandle(mChildHandle);
    mChildHandle = nullptr;
#else
    if (mChildFd < 0) {
        error = "worker channel child descriptor is not available";
        return false;
    }
    ::close(mChildFd);
    mChildFd = -1;
#endif
    return true;
}

bool WorkerDataChannel::send(llvm::StringRef data, std::string& error) {
    if (mDirection != WorkerChannelDirection::ParentToChild) {
        error = "cannot send a request on a child-to-parent channel";
        return false;
    }
#if defined(_WIN32)
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(mParentHandle, data.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            error = "request channel write failed with error " + std::to_string(GetLastError());
            closeParent();
            return false;
        }
        offset += written;
    }
#else
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t written = 0;
        do {
            written = ::send(mParentFd, data.data() + offset, data.size() - offset,
#if defined(MSG_NOSIGNAL)
                             MSG_NOSIGNAL
#else
                             0
#endif
            );
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            error = "request channel write failed with errno " + std::to_string(errno);
            closeParent();
            return false;
        }
        offset += static_cast<size_t>(written);
    }
#endif
    closeParent();
    return true;
}

bool WorkerDataChannel::drain(std::string& data, size_t maximumBytes, bool& closed,
                              std::string& error) {
    closed = false;
    if (mDirection != WorkerChannelDirection::ChildToParent) {
        error = "cannot drain a parent-to-child channel";
        return false;
    }
#if defined(_WIN32)
    if (!mParentHandle) {
        closed = true;
        return true;
    }
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(mParentHandle, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD channelError = GetLastError();
            if (channelError == ERROR_BROKEN_PIPE || channelError == ERROR_NO_DATA) {
                closeParent();
                closed = true;
                return true;
            }
            error = "result channel query failed with error " + std::to_string(channelError);
            return false;
        }
        if (available == 0) return true;
        std::array<char, 4096> buffer{};
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD count = 0;
        if (!ReadFile(mParentHandle, buffer.data(), requested, &count, nullptr) || count == 0) {
            const DWORD channelError = GetLastError();
            if (channelError == ERROR_BROKEN_PIPE || channelError == ERROR_NO_DATA) {
                closeParent();
                closed = true;
                return true;
            }
            error = "result channel read failed with error " + std::to_string(channelError);
            return false;
        }
        const size_t received = static_cast<size_t>(count);
        if (received > maximumBytes - std::min(maximumBytes, data.size())) {
            error = "result channel exceeded its protocol limit";
            return false;
        }
        data.append(buffer.data(), received);
    }
#else
    if (mParentFd < 0) {
        closed = true;
        return true;
    }
    while (true) {
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        do {
            count = ::recv(mParentFd, buffer.data(), buffer.size(), 0);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            const size_t received = static_cast<size_t>(count);
            if (received > maximumBytes - std::min(maximumBytes, data.size())) {
                error = "result channel exceeded its protocol limit";
                return false;
            }
            data.append(buffer.data(), received);
            continue;
        }
        if (count == 0) {
            closeParent();
            closed = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        error = "result channel read failed with errno " + std::to_string(errno);
        return false;
    }
#endif
}

bool WorkerDataChannel::drainToStream(std::ostream& output, uint64_t& remaining, bool& closed,
                                      bool& limitExceeded, size_t& transferred,
                                      std::string& error) {
    constexpr size_t MaximumTransferBytes = 64 * 1024;
    closed = false;
    limitExceeded = false;
    transferred = 0;
    if (mDirection != WorkerChannelDirection::ChildToParent) {
        error = "cannot drain output from a parent-to-child channel";
        return false;
    }
#if defined(_WIN32)
    if (!mParentHandle) {
        closed = true;
        return true;
    }
    while (transferred < MaximumTransferBytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(mParentHandle, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD channelError = GetLastError();
            if (channelError == ERROR_BROKEN_PIPE || channelError == ERROR_NO_DATA) {
                closeParent();
                closed = true;
                return true;
            }
            error = "output channel query failed with error " + std::to_string(channelError);
            return false;
        }
        if (available == 0) return true;
        if (remaining == 0) {
            limitExceeded = true;
            return true;
        }
        std::array<char, 4096> buffer{};
        const size_t budget = MaximumTransferBytes - transferred;
        const DWORD requested = static_cast<DWORD>(std::min<uint64_t>(
            std::min<size_t>(available, buffer.size()), std::min<uint64_t>(remaining, budget)));
        DWORD count = 0;
        if (!ReadFile(mParentHandle, buffer.data(), requested, &count, nullptr) || count == 0) {
            const DWORD channelError = GetLastError();
            if (channelError == ERROR_BROKEN_PIPE || channelError == ERROR_NO_DATA) {
                closeParent();
                closed = true;
                return true;
            }
            error = "output channel read failed with error " + std::to_string(channelError);
            return false;
        }
        output.write(buffer.data(), count);
        if (!output) {
            error = "parent output stream rejected worker output";
            return false;
        }
        transferred += count;
        remaining -= count;
        if (remaining == 0 && available > count) {
            limitExceeded = true;
            return true;
        }
    }
#else
    if (mParentFd < 0) {
        closed = true;
        return true;
    }
    while (transferred < MaximumTransferBytes) {
        if (remaining == 0) {
            char byte = 0;
            const ssize_t pending = ::recv(mParentFd, &byte, 1, MSG_PEEK);
            if (pending > 0) limitExceeded = true;
            if (pending == 0) {
                closeParent();
                closed = true;
            } else if (pending < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                error = "output channel query failed with errno " + std::to_string(errno);
                return false;
            }
            return true;
        }
        std::array<char, 4096> buffer{};
        const size_t budget = MaximumTransferBytes - transferred;
        const size_t requested = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), std::min<uint64_t>(remaining, budget)));
        ssize_t count = 0;
        do {
            count = ::recv(mParentFd, buffer.data(), requested, 0);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            output.write(buffer.data(), count);
            if (!output) {
                error = "parent output stream rejected worker output";
                return false;
            }
            transferred += static_cast<size_t>(count);
            remaining -= static_cast<uint64_t>(count);
            continue;
        }
        if (count == 0) {
            closeParent();
            closed = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        error = "output channel read failed with errno " + std::to_string(errno);
        return false;
    }
#endif
    return true;
}

void WorkerDataChannel::closeParent() {
#if defined(_WIN32)
    if (mParentHandle) CloseHandle(mParentHandle);
    mParentHandle = nullptr;
#else
    if (mParentFd >= 0) ::close(mParentFd);
    mParentFd = -1;
#endif
}

void closeWorkerChannel(uint64_t token) {
#if defined(_WIN32)
    if (token <= static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max()))
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token)));
#else
    if (token <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
        ::close(static_cast<int>(token));
#endif
}

bool writeWorkerChannel(uint64_t token, llvm::StringRef data) {
#if defined(_WIN32)
    if (token > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) return false;
    HANDLE channel = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token));
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(channel, data.data() + offset, requested, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
#else
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    const int channel = static_cast<int>(token);
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t written = 0;
        do {
            written = ::send(channel, data.data() + offset, data.size() - offset,
#if defined(MSG_NOSIGNAL)
                             MSG_NOSIGNAL
#else
                             0
#endif
            );
        } while (written < 0 && errno == EINTR);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
#endif
    return true;
}

bool readWorkerChannel(uint64_t token, size_t maximumBytes, std::string& data) {
    bool succeeded = true;
#if defined(_WIN32)
    if (token > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) return false;
    HANDLE channel = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token));
    while (true) {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        if (!ReadFile(channel, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) succeeded = false;
            break;
        }
        if (count == 0) break;
        const size_t received = static_cast<size_t>(count);
        if (received > maximumBytes - std::min(maximumBytes, data.size())) {
            succeeded = false;
            break;
        }
        data.append(buffer.data(), received);
    }
#else
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    const int channel = static_cast<int>(token);
    while (true) {
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        do {
            count = ::recv(channel, buffer.data(), buffer.size(), 0);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            succeeded = false;
            break;
        }
        if (count == 0) break;
        const size_t received = static_cast<size_t>(count);
        if (received > maximumBytes - std::min(maximumBytes, data.size())) {
            succeeded = false;
            break;
        }
        data.append(buffer.data(), received);
    }
#endif
    closeWorkerChannel(token);
    return succeeded;
}

bool publishWorkerSignal(uint64_t token, llvm::StringRef name, std::ostream& errors) {
#if defined(_WIN32)
    HANDLE signal = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token));
    if (!SetEvent(signal)) {
        errors << "error[repl-worker]: cannot publish " << name.str() << " signal: error "
               << GetLastError() << '\n';
        CloseHandle(signal);
        return false;
    }
    CloseHandle(signal);
#else
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        errors << "error[repl-worker]: invalid " << name.str() << " signal descriptor\n";
        return false;
    }
    const int signal = static_cast<int>(token);
    const char value = 1;
    ssize_t written = 0;
    do {
        written = ::send(signal, &value, 1,
#if defined(MSG_NOSIGNAL)
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
    } while (written < 0 && errno == EINTR);
    const int signalError = written == 1 ? 0 : errno;
    ::close(signal);
    if (written != 1) {
        errors << "error[repl-worker]: cannot publish " << name.str() << " signal: errno "
               << signalError << '\n';
        return false;
    }
#endif
    return true;
}

} // namespace luna::driver::repl_detail
