#pragma once

#include <llvm/ADT/StringRef.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace luna::driver::repl_detail {

inline constexpr size_t MaxReplCellBytes = 1024 * 1024;
inline constexpr size_t MaxReplDeclarationBytes = 8 * 1024 * 1024;
// Persisted declaration accounting excludes the newline separators added during
// recompilation. Keep a separate hard IPC bound with enough room for those
// separators and the synthetic entry point.
inline constexpr size_t MaxReplWorkerSourceBytes =
    MaxReplCellBytes + (2 * MaxReplDeclarationBytes) + 4096;
inline constexpr size_t MaxReplWorkerErrorBytes = 64 * 1024;
inline constexpr size_t MaxReplWorkerResultBytes = 2 * (MaxReplWorkerErrorBytes + 512);

enum class ReplWorkerStatus {
    Executed = 0,
    Rejected = 1,
    InfrastructureError = 2,
    Running = 3,
};

struct ReplWorkerTimings {
    uint64_t requestMicroseconds = 0;
    uint64_t linkLoadMicroseconds = 0;
    uint64_t frontendMicroseconds = 0;
    uint64_t lexerMicroseconds = 0;
    uint64_t parserMicroseconds = 0;
    uint64_t semanticMicroseconds = 0;
    uint64_t traitMicroseconds = 0;
    uint64_t ownershipMicroseconds = 0;
    uint64_t indexingMicroseconds = 0;
    uint64_t loweringMicroseconds = 0;
    uint64_t verificationMicroseconds = 0;
    uint64_t sealingMicroseconds = 0;
    uint64_t optimizationMicroseconds = 0;
    uint64_t codegenMicroseconds = 0;
    uint64_t codegenTotalMicroseconds = 0;
    uint64_t jitMaterializationMicroseconds = 0;
    uint64_t jitLookupMicroseconds = 0;
    uint64_t executionMicroseconds = 0;
    uint64_t jitCleanupMicroseconds = 0;
    uint64_t jitTotalMicroseconds = 0;
    uint64_t runningPublishMicroseconds = 0;
    uint64_t workerTotalMicroseconds = 0;
};

enum class WorkerResultParseResult {
    Parsed,
    Incomplete,
    Invalid,
};

std::string serializeWorkerRequest(llvm::StringRef operation, llvm::StringRef virtualPath,
                                   llvm::StringRef source);
bool parseWorkerRequest(llvm::StringRef request, std::string& operation, std::string& virtualPath,
                        std::string& source);

std::string serializeWorkerResult(ReplWorkerStatus status, int exitCode,
                                  const ReplWorkerTimings& timings, llvm::StringRef error);
WorkerResultParseResult parseWorkerResult(llvm::StringRef input, size_t& offset,
                                          ReplWorkerStatus& status, int& exitCode,
                                          ReplWorkerTimings& timings, std::string& error);

} // namespace luna::driver::repl_detail
