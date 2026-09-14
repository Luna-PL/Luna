#pragma once

#include <llvm/Support/Program.h>

#include <cstdint>
#include <iosfwd>
#include <string>

namespace luna::driver::repl_detail {

class ProcessTreeGuard {
public:
    ProcessTreeGuard(const llvm::sys::ProcessInfo& process, uint64_t memoryLimitBytes);
    ProcessTreeGuard(const ProcessTreeGuard&) = delete;
    ProcessTreeGuard& operator=(const ProcessTreeGuard&) = delete;
    ~ProcessTreeGuard();

    bool attach(std::string& error);
    void terminate();

private:
    llvm::sys::ProcessInfo mProcess;
    uint64_t mMemoryLimitBytes = 0;
    bool mAttached = false;
#if defined(_WIN32)
    void* mJob = nullptr;
#endif
};

bool protectWorkerHandles(uint64_t requestChannel, uint64_t resultChannel, uint64_t stdoutChannel,
                          uint64_t stderrChannel, uint64_t readySignal, uint64_t gateSignal,
                          uint64_t completionSignal, std::ostream& errors);
bool redirectWorkerOutput(uint64_t stdoutChannel, uint64_t stderrChannel, std::ostream& errors);
void closeWorkerOutput();
bool applyWorkerMemoryLimit(unsigned memoryLimitMiB, std::ostream& errors);
bool waitForWorkerGate(uint64_t readySignal, uint64_t gateSignal, unsigned parentProcessId,
                       std::ostream& errors);

} // namespace luna::driver::repl_detail
