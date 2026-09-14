#include "driver/Repl.h"

#include "driver/ReplProcess.h"
#include "driver/ReplProtocol.h"
#include "driver/ReplTiming.h"
#include "driver/ReplTransport.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Program.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LUNA_REPL_ADDRESS_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LUNA_REPL_ADDRESS_SANITIZED)
#define LUNA_REPL_ADDRESS_SANITIZED 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace luna::driver {
namespace {

using namespace repl_detail;

constexpr auto ReplWorkerCleanupGrace = std::chrono::milliseconds(500);
constexpr size_t MaxRetiredReplWorkers = 2;

const char* optimizationArgument(LunaOptimizationLevel level) {
    switch (level) {
    case LunaOptimizationLevel::O0:
        return "-O0";
    case LunaOptimizationLevel::O2:
        return "-O2";
    case LunaOptimizationLevel::O3:
        return "-O3";
    }
    return "-O0";
}

} // namespace

class ReplSession::PreparedWorker {
public:
    PreparedWorker() = default;
    PreparedWorker(const PreparedWorker&) = delete;
    PreparedWorker& operator=(const PreparedWorker&) = delete;
    ~PreparedWorker() {
        if (reaped || process.Pid == llvm::sys::ProcessInfo::InvalidPid) return;
        if (processTree) processTree->terminate();
        llvm::sys::Wait(process, std::nullopt, nullptr, nullptr, false);
    }

    llvm::sys::ProcessInfo poll(std::string* error = nullptr) {
        const auto waited = llvm::sys::Wait(process, 0u, error, nullptr, true);
        if (waited.Pid != llvm::sys::ProcessInfo::InvalidPid) reaped = true;
        return waited;
    }

    llvm::sys::ProcessInfo wait() {
        const auto waited = llvm::sys::Wait(process, std::nullopt, nullptr, nullptr, false);
        reaped = true;
        return waited;
    }

    WorkerDataChannel requestChannel;
    WorkerDataChannel resultChannel;
    WorkerDataChannel stdoutChannel;
    WorkerDataChannel stderrChannel;
    WorkerSignal readySignal;
    WorkerSignal gateSignal;
    WorkerSignal completionSignal;
    llvm::sys::ProcessInfo process;
    std::unique_ptr<ProcessTreeGuard> processTree;
    std::string processError;
    std::string resultData;
    bool executionFailed = false;
    bool containmentAttached = false;
    bool readyObserved = false;
    bool reaped = false;
    std::chrono::steady_clock::time_point cleanupDeadline;
};

class ReplSession::WorkerReaper {
public:
    WorkerReaper() : mThread([this] { run(); }) {}
    WorkerReaper(const WorkerReaper&) = delete;
    WorkerReaper& operator=(const WorkerReaper&) = delete;
    ~WorkerReaper() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mWorkAvailable.notify_all();
        mSpaceAvailable.notify_all();
        if (mThread.joinable()) mThread.join();
    }

    void retire(std::unique_ptr<PreparedWorker> worker) {
        std::unique_lock<std::mutex> lock(mMutex);
        mSpaceAvailable.wait(
            lock, [this] { return mStopping || mOutstandingWorkers < MaxRetiredReplWorkers; });
        if (mStopping) {
            lock.unlock();
            worker.reset();
            return;
        }
        ++mOutstandingWorkers;
        mQueue.push_back(std::move(worker));
        lock.unlock();
        mWorkAvailable.notify_one();
    }

private:
    bool stopping() {
        std::lock_guard<std::mutex> lock(mMutex);
        return mStopping;
    }

    void reap(PreparedWorker& worker) {
        while (true) {
            if (worker.poll().Pid != llvm::sys::ProcessInfo::InvalidPid) return;
            if (stopping() || std::chrono::steady_clock::now() >= worker.cleanupDeadline) {
                worker.processTree->terminate();
                worker.wait();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void run() {
        while (true) {
            std::unique_ptr<PreparedWorker> worker;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWorkAvailable.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                if (mQueue.empty()) {
                    if (mStopping) return;
                    continue;
                }
                worker = std::move(mQueue.front());
                mQueue.pop_front();
            }

            reap(*worker);
            worker.reset();

            {
                std::lock_guard<std::mutex> lock(mMutex);
                --mOutstandingWorkers;
            }
            mSpaceAvailable.notify_one();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWorkAvailable;
    std::condition_variable mSpaceAvailable;
    std::deque<std::unique_ptr<PreparedWorker>> mQueue;
    size_t mOutstandingWorkers = 0;
    bool mStopping = false;
    std::thread mThread;
};

class ReplSession::WorkerPreparer {
public:
    explicit WorkerPreparer(const ReplSession& session) : mSession(session) {}
    WorkerPreparer(const WorkerPreparer&) = delete;
    WorkerPreparer& operator=(const WorkerPreparer&) = delete;
    ~WorkerPreparer() {
        if (!mFuture.valid()) return;
        try {
            mFuture.get();
        } catch (...) {}
    }

    void start() {
        if (mFuture.valid() || !mStartError.empty()) return;
        try {
            mFuture = std::async(std::launch::async, [this] {
                Result result;
                std::ostringstream errors;
                result.worker = mSession.createWorker(errors);
                result.error = errors.str();
                return result;
            });
        } catch (const std::exception& error) { mStartError = error.what(); } catch (...) {
            mStartError = "unknown exception";
        }
    }

    std::unique_ptr<PreparedWorker> acquire(std::ostream& errors) {
        start();
        if (!mStartError.empty()) {
            errors << "error[repl]: cannot start background worker preparation: " << mStartError
                   << '\n';
            mStartError.clear();
            return nullptr;
        }
        try {
            Result result = mFuture.get();
            if (!result.error.empty()) errors << result.error;
            return std::move(result.worker);
        } catch (const std::exception& error) {
            errors << "error[repl]: worker preparation failed: " << error.what() << '\n';
        } catch (...) {
            errors << "error[repl]: worker preparation failed with an unknown exception\n";
        }
        return nullptr;
    }

private:
    struct Result {
        std::unique_ptr<PreparedWorker> worker;
        std::string error;
    };

    const ReplSession& mSession;
    std::future<Result> mFuture;
    std::string mStartError;
};

ReplSession::ReplSession(ReplOptions options) : mOptions(std::move(options)) {}
ReplSession::~ReplSession() {
    mWorkerPreparer.reset();
    mPreparedWorker.reset();
    mWorkerReaper.reset();
}

void ReplSession::retireWorker(std::unique_ptr<PreparedWorker> worker) const {
    try {
        if (!mWorkerReaper) mWorkerReaper = std::make_unique<WorkerReaper>();
        worker->cleanupDeadline = std::chrono::steady_clock::now() + ReplWorkerCleanupGrace;
        mWorkerReaper->retire(std::move(worker));
    } catch (...) { worker.reset(); }
}

bool ReplSession::prepareWorker(std::ostream& errors) const {
    if (mPreparedWorker) return true;
    if (mOptions.workerExecutable.empty()) return false;

    if (!mWorkerPreparer) mWorkerPreparer = std::make_unique<WorkerPreparer>(*this);
    mPreparedWorker = mWorkerPreparer->acquire(errors);
    return mPreparedWorker != nullptr;
}

void ReplSession::startPreparingWorker() const {
    if (mOptions.workerExecutable.empty()) return;
    if (!mWorkerPreparer) mWorkerPreparer = std::make_unique<WorkerPreparer>(*this);
    mWorkerPreparer->start();
}

std::unique_ptr<ReplSession::PreparedWorker> ReplSession::createWorker(std::ostream& errors) const {

    auto worker = std::make_unique<PreparedWorker>();
    if (!worker->requestChannel.create(WorkerChannelDirection::ParentToChild,
                                       worker->processError) ||
        !worker->resultChannel.create(WorkerChannelDirection::ChildToParent,
                                      worker->processError) ||
        !worker->stdoutChannel.create(WorkerChannelDirection::ChildToParent,
                                      worker->processError) ||
        !worker->stderrChannel.create(WorkerChannelDirection::ChildToParent,
                                      worker->processError) ||
        !worker->readySignal.create(true, worker->processError) ||
        !worker->gateSignal.create(false, worker->processError) ||
        !worker->completionSignal.create(true, worker->processError)) {
        errors << "error[repl]: cannot create worker channel: " << worker->processError << '\n';
        return nullptr;
    }
#if defined(LUNA_REPL_ADDRESS_SANITIZED)
    constexpr unsigned containmentMemoryLimitMiB = 0;
#else
    const unsigned containmentMemoryLimitMiB = mOptions.executionMemoryLimitMiB;
#endif

    std::vector<std::string> arguments;
    arguments.reserve(18 + mOptions.linkLibraries.size() * 2);
    arguments.push_back(mOptions.workerExecutable);
    arguments.emplace_back("__repl-worker");
    arguments.emplace_back("--request-channel");
    arguments.push_back(std::to_string(worker->requestChannel.childToken()));
    arguments.emplace_back("--result-channel");
    arguments.push_back(std::to_string(worker->resultChannel.childToken()));
    arguments.emplace_back("--stdout-channel");
    arguments.push_back(std::to_string(worker->stdoutChannel.childToken()));
    arguments.emplace_back("--stderr-channel");
    arguments.push_back(std::to_string(worker->stderrChannel.childToken()));
    arguments.emplace_back("--ready");
    arguments.push_back(std::to_string(worker->readySignal.childToken()));
    arguments.emplace_back("--gate");
    arguments.push_back(std::to_string(worker->gateSignal.childToken()));
    arguments.emplace_back("--complete");
    arguments.push_back(std::to_string(worker->completionSignal.childToken()));
    arguments.emplace_back("--parent-pid");
#if defined(_WIN32)
    arguments.push_back(std::to_string(GetCurrentProcessId()));
#else
    arguments.push_back(std::to_string(::getpid()));
#endif
    arguments.emplace_back("--worker-memory-limit");
    arguments.push_back(std::to_string(containmentMemoryLimitMiB));
    arguments.emplace_back(optimizationArgument(mOptions.optimizationLevel));
    for (const auto& library : mOptions.linkLibraries) {
        arguments.emplace_back("--link");
        arguments.push_back(library);
    }
    std::vector<llvm::StringRef> argumentRefs;
    argumentRefs.reserve(arguments.size());
    for (const auto& argument : arguments)
        argumentRefs.emplace_back(argument);

#if defined(_WIN32)
    constexpr llvm::StringLiteral NullDevice = "NUL";
#else
    constexpr llvm::StringLiteral NullDevice = "/dev/null";
#endif
    // Suppress startup diagnostics until the worker installs its inherited
    // output channels. Non-empty redirects also keep Windows handle inheritance
    // enabled in LLVM's process launcher.
    const std::array<std::optional<llvm::StringRef>, 3> redirects = {llvm::StringRef(), NullDevice,
                                                                     NullDevice};
    // LLVM's Unix launcher falls back to fork() when this argument is nonzero.
    // Worker preparation runs beside the reaper thread, so keep the launcher on
    // posix_spawn() and install the inherited RLIMIT_AS inside the worker before
    // it publishes readiness. Windows uses the REPL-owned Job Object below.
    constexpr unsigned launchMemoryLimitMiB = 0;
    worker->process = llvm::sys::ExecuteNoWait(mOptions.workerExecutable, argumentRefs,
                                               std::nullopt, redirects, launchMemoryLimitMiB,
                                               &worker->processError, &worker->executionFailed);
    if (worker->process.Pid != llvm::sys::ProcessInfo::InvalidPid)
        worker->processTree = std::make_unique<ProcessTreeGuard>(
            worker->process, static_cast<uint64_t>(containmentMemoryLimitMiB) * 1024 * 1024);
    std::string channelError;
    const bool requestFinalized = worker->requestChannel.finishLaunch(channelError);
    const bool resultFinalized = worker->resultChannel.finishLaunch(channelError);
    const bool stdoutFinalized = worker->stdoutChannel.finishLaunch(channelError);
    const bool stderrFinalized = worker->stderrChannel.finishLaunch(channelError);
    const bool readyFinalized = worker->readySignal.finishLaunch(channelError);
    const bool gateFinalized = worker->gateSignal.finishLaunch(channelError);
    const bool completionFinalized = worker->completionSignal.finishLaunch(channelError);
    if (!requestFinalized || !resultFinalized || !stdoutFinalized || !stderrFinalized ||
        !readyFinalized || !gateFinalized || !completionFinalized) {
        errors << "error[repl]: cannot finalize worker channels: " << channelError << '\n';
        return nullptr;
    }
    if (worker->executionFailed || worker->process.Pid == llvm::sys::ProcessInfo::InvalidPid) {
        errors << "error[repl]: cannot start execution worker";
        if (!worker->processError.empty()) errors << ": " << worker->processError;
        errors << '\n';
        return nullptr;
    }
    return worker;
}

bool ReplSession::runInWorker(const std::string& source, const std::string& virtualPath,
                              const std::string& operation, int& result, std::string& payload,
                              std::ostream& output, std::ostream& errors) const {
    ReplTimingReporter timingReporter(mOptions.showTimings, operation, errors);
    if (mOptions.workerExecutable.empty()) {
        errors << "error[repl]: execution worker is not configured\n";
        return false;
    }
    if (mOptions.executionTimeoutSeconds == 0 || mOptions.executionTimeoutSeconds > 3600) {
        errors << "error[repl]: execution timeout must be between 1 and 3600 seconds\n";
        return false;
    }
    if (mOptions.executionMemoryLimitMiB < 256 || mOptions.executionMemoryLimitMiB > 65536) {
        errors << "error[repl]: worker memory limit must be between 256 and 65536 MiB\n";
        return false;
    }
    if (mOptions.executionOutputLimitMiB == 0 || mOptions.executionOutputLimitMiB > 1024) {
        errors << "error[repl]: worker output limit must be between 1 and 1024 MiB\n";
        return false;
    }
    if (operation == "type" && findCachedTypeResult(source, payload)) {
        result = 0;
        timingReporter.setCacheHit();
        return true;
    }

    if (!mPreparedWorker && !prepareWorker(errors)) return false;
    auto worker = std::move(mPreparedWorker);
    std::string initialSignalError;
    const auto initialReady =
        worker->readySignal.wait(std::chrono::milliseconds(0), initialSignalError);
    worker->readyObserved = initialReady == SignalWaitResult::Signaled;
    if (initialReady == SignalWaitResult::Failed) worker->processError = initialSignalError;
    timingReporter.setWorkerPrewarmed(worker->readyObserved);
    timingReporter.markWorkerAcquired();
    const std::string requestData = serializeWorkerRequest(operation, virtualPath, source);

    bool completionPublished = false;
    const bool succeeded = [&]() -> bool {
        enum class TerminationReason {
            None,
            Timeout,
            OutputLimit,
            ContainmentFailure,
            ProtocolFailure,
        };
        TerminationReason terminationReason = TerminationReason::None;
        int processResult = -1;
        const uint64_t outputLimit =
            static_cast<uint64_t>(mOptions.executionOutputLimitMiB) * 1024 * 1024;
        uint64_t remainingOutput = outputLimit;
        bool outputTruncated = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(mOptions.executionTimeoutSeconds);
        const auto drainWorkerResults = [&]() {
            bool channelClosed = false;
            std::string channelError;
            if (!worker->resultChannel.drain(worker->resultData, MaxReplWorkerResultBytes,
                                             channelClosed, channelError)) {
                worker->processError = std::move(channelError);
                return false;
            }
            return true;
        };
        const auto drainWorkerOutput = [&](bool drainFully) {
            constexpr size_t MaximumFinalDrainPasses = 16;
            size_t passes = 0;
            while (true) {
                size_t transferred = 0;
                for (const auto& channelAndStream : {std::pair{&worker->stderrChannel, &errors},
                                                     std::pair{&worker->stdoutChannel, &output}}) {
                    bool channelClosed = false;
                    bool limitExceeded = false;
                    size_t channelTransferred = 0;
                    std::string channelError;
                    if (!channelAndStream.first->drainToStream(
                            *channelAndStream.second, remainingOutput, channelClosed, limitExceeded,
                            channelTransferred, channelError)) {
                        worker->processError = std::move(channelError);
                        return TerminationReason::ProtocolFailure;
                    }
                    transferred += channelTransferred;
                    if (limitExceeded) {
                        outputTruncated = true;
                        return TerminationReason::OutputLimit;
                    }
                }
                ++passes;
                if (!drainFully || transferred == 0 || passes >= MaximumFinalDrainPasses)
                    return TerminationReason::None;
            }
        };
        if (!worker->executionFailed && worker->process.Pid != llvm::sys::ProcessInfo::InvalidPid) {
            while (true) {
                if (!drainWorkerResults()) {
                    terminationReason = TerminationReason::ProtocolFailure;
                    worker->processTree->terminate();
                    processResult = worker->wait().ReturnCode;
                    break;
                }
                const auto outputDrain = drainWorkerOutput(false);
                if (outputDrain != TerminationReason::None) {
                    terminationReason = outputDrain;
                    worker->processTree->terminate();
                    processResult = worker->wait().ReturnCode;
                    break;
                }
                if (!worker->containmentAttached && !worker->readyObserved) {
                    std::string signalError;
                    const auto ready =
                        worker->readySignal.wait(std::chrono::milliseconds(0), signalError);
                    if (ready == SignalWaitResult::Signaled)
                        worker->readyObserved = true;
                    else if (ready == SignalWaitResult::Failed) {
                        worker->processError = std::move(signalError);
                        terminationReason = TerminationReason::ContainmentFailure;
                        worker->processTree->terminate();
                        processResult = worker->wait().ReturnCode;
                        break;
                    }
                }
                if (!worker->containmentAttached && worker->readyObserved) {
                    if (!worker->processTree->attach(worker->processError) ||
                        !worker->gateSignal.signal(worker->processError)) {
                        if (worker->processError.empty())
                            worker->processError = "cannot open worker start gate";
                        terminationReason = TerminationReason::ContainmentFailure;
                        worker->processTree->terminate();
                        processResult = worker->wait().ReturnCode;
                        break;
                    }
                    worker->containmentAttached = true;
                    if (!worker->requestChannel.send(requestData, worker->processError)) {
                        terminationReason = TerminationReason::ProtocolFailure;
                        worker->processTree->terminate();
                        processResult = worker->wait().ReturnCode;
                        break;
                    }
                    timingReporter.markRequestSubmitted();
                }

                std::string completionError;
                const auto completion =
                    worker->completionSignal.wait(std::chrono::milliseconds(0), completionError);
                if (completion == SignalWaitResult::Signaled) {
                    completionPublished = true;
                    processResult = 0;
                    break;
                }
                if (completion == SignalWaitResult::Failed) {
                    worker->processError = std::move(completionError);
                    terminationReason = TerminationReason::ContainmentFailure;
                    worker->processTree->terminate();
                    processResult = worker->wait().ReturnCode;
                    break;
                }

                std::string waitError;
                const auto waited = worker->poll(&waitError);
                if (!waitError.empty() && worker->processError.empty())
                    worker->processError = waitError;
                if (waited.Pid != llvm::sys::ProcessInfo::InvalidPid) {
                    processResult = waited.ReturnCode;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    terminationReason = TerminationReason::Timeout;
                    worker->processTree->terminate();
                    processResult = worker->wait().ReturnCode;
                    break;
                }

                std::string signalError;
                auto signalResult = SignalWaitResult::TimedOut;
                if (worker->containmentAttached)
                    signalResult =
                        worker->completionSignal.wait(std::chrono::milliseconds(10), signalError);
                else
                    signalResult =
                        worker->readySignal.wait(std::chrono::milliseconds(10), signalError);
                if (signalResult == SignalWaitResult::Signaled) {
                    if (worker->containmentAttached) {
                        completionPublished = true;
                        processResult = 0;
                        break;
                    }
                    worker->readyObserved = true;
                } else if (signalResult == SignalWaitResult::Failed) {
                    worker->processError = std::move(signalError);
                    terminationReason = TerminationReason::ContainmentFailure;
                    worker->processTree->terminate();
                    processResult = worker->wait().ReturnCode;
                    break;
                }
            }
        }
        timingReporter.markWorkerFinished();
        if (terminationReason == TerminationReason::None && worker->containmentAttached &&
            !completionPublished)
            worker->processTree->terminate();
        if (!drainWorkerResults() && terminationReason == TerminationReason::None)
            terminationReason = TerminationReason::ProtocolFailure;
        const auto finalOutputDrain = drainWorkerOutput(true);
        if (terminationReason == TerminationReason::None &&
            finalOutputDrain != TerminationReason::None) {
            terminationReason = finalOutputDrain;
            if (!worker->reaped) {
                worker->processTree->terminate();
                processResult = worker->wait().ReturnCode;
            }
        }
        if (outputTruncated)
            errors << "error[repl]: worker output was truncated at the "
                   << mOptions.executionOutputLimitMiB << " MiB limit\n";

        ReplWorkerStatus status = ReplWorkerStatus::InfrastructureError;
        int workerExitCode = 0;
        std::string workerError;
        ReplWorkerTimings workerTimings;
        bool hasFinalResult = false;
        ReplWorkerStatus intermediateStatus = ReplWorkerStatus::InfrastructureError;
        ReplWorkerTimings intermediateTimings;
        bool hasIntermediateResult = false;
        bool invalidResultProtocol = false;
        size_t resultOffset = 0;
        size_t resultFrames = 0;
        while (resultOffset < worker->resultData.size() && resultFrames < 2) {
            ReplWorkerStatus frameStatus = ReplWorkerStatus::InfrastructureError;
            int frameExitCode = 0;
            ReplWorkerTimings frameTimings;
            std::string frameError;
            const auto parsed = parseWorkerResult(worker->resultData, resultOffset, frameStatus,
                                                  frameExitCode, frameTimings, frameError);
            if (parsed == WorkerResultParseResult::Incomplete) {
                invalidResultProtocol = completionPublished;
                break;
            }
            if (parsed == WorkerResultParseResult::Invalid) {
                invalidResultProtocol = true;
                break;
            }
            ++resultFrames;
            if (frameStatus == ReplWorkerStatus::Running) {
                if (hasIntermediateResult || hasFinalResult) {
                    invalidResultProtocol = true;
                    break;
                }
                hasIntermediateResult = true;
                intermediateStatus = frameStatus;
                intermediateTimings = frameTimings;
            } else {
                if (hasFinalResult) {
                    invalidResultProtocol = true;
                    break;
                }
                hasFinalResult = true;
                status = frameStatus;
                workerExitCode = frameExitCode;
                workerTimings = frameTimings;
                workerError = std::move(frameError);
            }
        }
        if ((resultFrames == 2 || completionPublished) && resultOffset != worker->resultData.size())
            invalidResultProtocol = true;
        if (completionPublished && !hasFinalResult) invalidResultProtocol = true;
        if (hasFinalResult)
            timingReporter.setWorkerTimings(workerTimings);
        else if (hasIntermediateResult)
            timingReporter.setWorkerTimings(intermediateTimings);
        if (terminationReason == TerminationReason::Timeout) {
            errors << "error[repl]: worker compilation or execution exceeded the "
                   << mOptions.executionTimeoutSeconds << " second timeout\n";
            return false;
        }
        if (terminationReason == TerminationReason::OutputLimit) {
            errors << "error[repl]: worker exceeded the " << mOptions.executionOutputLimitMiB
                   << " MiB output limit\n";
            return false;
        }
        if (terminationReason == TerminationReason::ContainmentFailure) {
            errors << "error[repl]: cannot establish worker process containment";
            if (!worker->processError.empty()) errors << ": " << worker->processError;
            errors << '\n';
            return false;
        }
        if (terminationReason == TerminationReason::ProtocolFailure || invalidResultProtocol) {
            errors << "error[repl]: worker data channel failed";
            if (!worker->processError.empty()) errors << ": " << worker->processError;
            errors << '\n';
            return false;
        }
        if (worker->executionFailed || worker->process.Pid == llvm::sys::ProcessInfo::InvalidPid) {
            errors << "error[repl]: cannot start execution worker";
            if (!worker->processError.empty()) errors << ": " << worker->processError;
            errors << '\n';
            return false;
        }
        if (!hasFinalResult) {
            const bool wasRunning =
                hasIntermediateResult && intermediateStatus == ReplWorkerStatus::Running;
            errors << "error[repl]: worker ";
            if (wasRunning)
                errors << "terminated while running JIT code";
            else if (processResult == -2)
                errors << "was killed before returning a result (possible memory limit)";
            else
                errors << "terminated before returning a result";
            errors << '\n';
            return false;
        }
        if (status == ReplWorkerStatus::Rejected) return false;
        if (status == ReplWorkerStatus::InfrastructureError) {
            errors << "error[repl]: execution worker failed";
            if (!workerError.empty()) errors << ": " << workerError;
            errors << '\n';
            return false;
        }
        if (status != ReplWorkerStatus::Executed) {
            errors << "error[repl]: execution worker returned an invalid final state\n";
            return false;
        }
        result = workerExitCode;
        payload = std::move(workerError);
        if (operation == "type") rememberTypeResult(source, payload);
        return true;
    }();
    timingReporter.markResultHandled();

    if (completionPublished && !worker->reaped)
        retireWorker(std::move(worker));
    else
        worker.reset();
    startPreparingWorker();
    return succeeded;
}

} // namespace luna::driver
