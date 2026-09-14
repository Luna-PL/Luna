#pragma once

#include "codegen/CodeGenerator.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace luna::driver {

struct ReplOptions {
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
    bool showPrompts = true;
    bool showTimings = false;
    std::string workerExecutable;
    std::vector<std::string> linkLibraries;
    unsigned executionTimeoutSeconds = 30;
    unsigned executionMemoryLimitMiB = 1024;
    unsigned executionOutputLimitMiB = 16;
};

class ReplSession {
public:
    explicit ReplSession(ReplOptions options = {});
    ~ReplSession();

    int run(std::istream& input, std::ostream& output, std::ostream& errors);

private:
    struct CachedTypeResult {
        std::string source;
        std::string type;
    };

    const std::string& declarationsSource() const;
    std::string nextVirtualPath();
    bool storeDeclaration(const std::string& declaration, const std::string& virtualPath,
                          std::ostream& output, std::ostream& errors);
    bool evaluateExpression(const std::string& expression, const std::string& virtualPath,
                            std::ostream& output, std::ostream& errors) const;
    bool executeStatement(const std::string& statement, const std::string& virtualPath,
                          std::ostream& output, std::ostream& errors) const;
    bool inspectExpressionType(const std::string& expression, const std::string& virtualPath,
                               std::ostream& output, std::ostream& errors) const;
    bool compileAndRun(const std::string& source, const std::string& virtualPath, int& result,
                       std::ostream& output, std::ostream& errors) const;
    bool runInWorker(const std::string& source, const std::string& virtualPath,
                     const std::string& operation, int& result, std::string& payload,
                     std::ostream& output, std::ostream& errors) const;
    bool prepareWorker(std::ostream& errors) const;
    void startPreparingWorker() const;

    class PreparedWorker;
    std::unique_ptr<PreparedWorker> createWorker(std::ostream& errors) const;
    class WorkerPreparer;
    class WorkerReaper;
    void retireWorker(std::unique_ptr<PreparedWorker> worker) const;
    bool findCachedTypeResult(const std::string& source, std::string& type) const;
    void rememberTypeResult(const std::string& source, const std::string& type) const;
    void clearTypeCache();

    ReplOptions mOptions;
    std::vector<std::string> mDeclarations;
    std::string mDeclarationsSource;
    size_t mDeclarationBytes = 0;
    size_t mNextCell = 1;
    mutable std::unique_ptr<PreparedWorker> mPreparedWorker;
    mutable std::unique_ptr<WorkerPreparer> mWorkerPreparer;
    mutable std::unique_ptr<WorkerReaper> mWorkerReaper;
    mutable std::vector<CachedTypeResult> mTypeCache;
    mutable size_t mTypeCacheBytes = 0;
};

void printReplUsage(std::ostream& output);
int runReplWorker(std::ostream& errors, const std::vector<std::string>& linkLibraries,
                  uint64_t requestChannel, uint64_t resultChannel, uint64_t readySignal,
                  uint64_t gateSignal, uint64_t completionSignal, uint64_t stdoutChannel,
                  uint64_t stderrChannel, unsigned parentProcessId, unsigned memoryLimitMiB,
                  LunaOptimizationLevel optimizationLevel);
int runRepl(std::istream& input, std::ostream& output, std::ostream& errors,
            ReplOptions options = {});

} // namespace luna::driver
