#include "driver/ReplProtocol.h"

#include <iostream>
#include <string>

namespace {

using namespace luna::driver::repl_detail;

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "repl protocol test failed: " << message << '\n';
    return false;
}

} // namespace

int main() {
    const std::string virtualPath = "<repl:test>";
    std::string source = "fn main() -> i32 {\nreturn 7;\n}";
    source.push_back('\0');
    source += "tail";
    const std::string request = serializeWorkerRequest("run", virtualPath, source);
    std::string parsedOperation;
    std::string parsedPath;
    std::string parsedSource;
    if (!require(parseWorkerRequest(request, parsedOperation, parsedPath, parsedSource),
                 "valid request was rejected") ||
        !require(parsedOperation == "run", "request operation changed") ||
        !require(parsedPath == virtualPath, "request path changed") ||
        !require(parsedSource == source, "request source changed"))
        return 1;

    if (!require(!parseWorkerRequest(serializeWorkerRequest("unknown", virtualPath, source),
                                     parsedOperation, parsedPath, parsedSource),
                 "unknown request operation was accepted") ||
        !require(!parseWorkerRequest(request + "x", parsedOperation, parsedPath, parsedSource),
                 "request with trailing bytes was accepted") ||
        !require(!parseWorkerRequest("LUNA_REPL_REQUEST_V1\nrun\n1\n9\nxshort", parsedOperation,
                                     parsedPath, parsedSource),
                 "truncated request was accepted"))
        return 1;

    ReplWorkerTimings timings;
    timings.requestMicroseconds = 11;
    timings.linkLoadMicroseconds = 12;
    timings.frontendMicroseconds = 13;
    timings.lexerMicroseconds = 14;
    timings.parserMicroseconds = 15;
    timings.semanticMicroseconds = 16;
    timings.traitMicroseconds = 17;
    timings.ownershipMicroseconds = 18;
    timings.indexingMicroseconds = 19;
    timings.loweringMicroseconds = 20;
    timings.verificationMicroseconds = 21;
    timings.sealingMicroseconds = 22;
    timings.optimizationMicroseconds = 23;
    timings.codegenMicroseconds = 24;
    timings.codegenTotalMicroseconds = 25;
    timings.jitMaterializationMicroseconds = 26;
    timings.jitLookupMicroseconds = 27;
    timings.executionMicroseconds = 28;
    timings.jitCleanupMicroseconds = 29;
    timings.jitTotalMicroseconds = 30;
    timings.runningPublishMicroseconds = 31;
    timings.workerTotalMicroseconds = 32;
    const std::string first = serializeWorkerResult(ReplWorkerStatus::Running, 0, timings, {});
    const std::string error("failure\0detail", 14);
    const std::string second =
        serializeWorkerResult(ReplWorkerStatus::InfrastructureError, -7, timings, error);
    const std::string frames = first + second;
    size_t offset = 0;
    ReplWorkerStatus status = ReplWorkerStatus::Rejected;
    int exitCode = 0;
    ReplWorkerTimings parsedTimings;
    std::string parsedError;
    if (!require(parseWorkerResult(frames, offset, status, exitCode, parsedTimings, parsedError) ==
                     WorkerResultParseResult::Parsed,
                 "running frame was rejected") ||
        !require(status == ReplWorkerStatus::Running && offset == first.size(),
                 "running frame boundary changed") ||
        !require(parseWorkerResult(frames, offset, status, exitCode, parsedTimings, parsedError) ==
                     WorkerResultParseResult::Parsed,
                 "final frame was rejected") ||
        !require(status == ReplWorkerStatus::InfrastructureError && exitCode == -7,
                 "final frame status changed") ||
        !require(
            parsedTimings.requestMicroseconds == 11 && parsedTimings.linkLoadMicroseconds == 12 &&
                parsedTimings.frontendMicroseconds == 13 && parsedTimings.lexerMicroseconds == 14 &&
                parsedTimings.parserMicroseconds == 15 &&
                parsedTimings.semanticMicroseconds == 16 && parsedTimings.traitMicroseconds == 17 &&
                parsedTimings.ownershipMicroseconds == 18 &&
                parsedTimings.indexingMicroseconds == 19 &&
                parsedTimings.loweringMicroseconds == 20 &&
                parsedTimings.verificationMicroseconds == 21 &&
                parsedTimings.sealingMicroseconds == 22 &&
                parsedTimings.optimizationMicroseconds == 23 &&
                parsedTimings.codegenMicroseconds == 24 &&
                parsedTimings.codegenTotalMicroseconds == 25 &&
                parsedTimings.jitMaterializationMicroseconds == 26 &&
                parsedTimings.jitLookupMicroseconds == 27 &&
                parsedTimings.executionMicroseconds == 28 &&
                parsedTimings.jitCleanupMicroseconds == 29 &&
                parsedTimings.jitTotalMicroseconds == 30 &&
                parsedTimings.runningPublishMicroseconds == 31 &&
                parsedTimings.workerTotalMicroseconds == 32,
            "worker timings changed") ||
        !require(parsedError == error && offset == frames.size(), "worker error payload changed"))
        return 1;

    size_t incompleteOffset = 0;
    if (!require(parseWorkerResult(first.substr(0, first.size() - 1), incompleteOffset, status,
                                   exitCode, parsedTimings,
                                   parsedError) == WorkerResultParseResult::Incomplete,
                 "truncated result was not classified as incomplete"))
        return 1;

    std::string oversizedError(MaxReplWorkerErrorBytes + 17, 'e');
    const std::string bounded =
        serializeWorkerResult(ReplWorkerStatus::InfrastructureError, 0, timings, oversizedError);
    size_t boundedOffset = 0;
    if (!require(parseWorkerResult(bounded, boundedOffset, status, exitCode, parsedTimings,
                                   parsedError) == WorkerResultParseResult::Parsed,
                 "bounded error frame was rejected") ||
        !require(parsedError.size() == MaxReplWorkerErrorBytes, "worker error was not bounded") ||
        !require(parsedError.find("[worker result truncated]") != std::string::npos,
                 "worker error truncation was not marked"))
        return 1;

    size_t invalidOffset = 0;
    if (!require(parseWorkerResult("NOT_LUNA\n0\n0\n0\n0\n0\n0\n", invalidOffset, status, exitCode,
                                   parsedTimings, parsedError) == WorkerResultParseResult::Invalid,
                 "invalid result magic was accepted"))
        return 1;

    std::string invalidTiming = first;
    const size_t timingPosition = invalidTiming.find("\n11\n");
    if (!require(timingPosition != std::string::npos, "timing field fixture was not found"))
        return 1;
    invalidTiming.replace(timingPosition + 1, 2, "-1");
    invalidOffset = 0;
    if (!require(parseWorkerResult(invalidTiming, invalidOffset, status, exitCode, parsedTimings,
                                   parsedError) == WorkerResultParseResult::Invalid,
                 "negative timing field was accepted") ||
        !require(invalidOffset == 0, "invalid timing frame advanced the input offset"))
        return 1;

    return 0;
}
