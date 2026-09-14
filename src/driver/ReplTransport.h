#pragma once

#include <llvm/ADT/StringRef.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace luna::driver::repl_detail {

enum class SignalWaitResult {
    Signaled,
    TimedOut,
    Closed,
    Failed,
};

class WorkerSignal {
public:
    WorkerSignal() = default;
    WorkerSignal(const WorkerSignal&) = delete;
    WorkerSignal& operator=(const WorkerSignal&) = delete;
    ~WorkerSignal();

    bool create(bool childSignals, std::string& error);
    uint64_t childToken() const;
    bool finishLaunch(std::string& error);
    bool signal(std::string& error);
    SignalWaitResult wait(std::chrono::milliseconds timeout, std::string& error);

private:
    bool mChildSignals = false;
#if defined(_WIN32)
    void* mHandle = nullptr;
#else
    int mParentFd = -1;
    int mChildFd = -1;
#endif
};

enum class WorkerChannelDirection {
    ParentToChild,
    ChildToParent,
};

class WorkerDataChannel {
public:
    WorkerDataChannel() = default;
    WorkerDataChannel(const WorkerDataChannel&) = delete;
    WorkerDataChannel& operator=(const WorkerDataChannel&) = delete;
    ~WorkerDataChannel();

    bool create(WorkerChannelDirection direction, std::string& error);
    uint64_t childToken() const;
    bool finishLaunch(std::string& error);
    bool send(llvm::StringRef data, std::string& error);
    bool drain(std::string& data, size_t maximumBytes, bool& closed, std::string& error);
    bool drainToStream(std::ostream& output, uint64_t& remaining, bool& closed, bool& limitExceeded,
                       size_t& transferred, std::string& error);

private:
    void closeParent();

    WorkerChannelDirection mDirection = WorkerChannelDirection::ParentToChild;
#if defined(_WIN32)
    void* mParentHandle = nullptr;
    void* mChildHandle = nullptr;
#else
    int mParentFd = -1;
    int mChildFd = -1;
#endif
};

void closeWorkerChannel(uint64_t token);
bool writeWorkerChannel(uint64_t token, llvm::StringRef data);
bool readWorkerChannel(uint64_t token, size_t maximumBytes, std::string& data);
bool publishWorkerSignal(uint64_t token, llvm::StringRef name, std::ostream& errors);

} // namespace luna::driver::repl_detail
