#include "driver/ReplProtocol.h"

#include <array>
#include <charconv>
#include <sstream>

namespace luna::driver::repl_detail {
namespace {

constexpr char ReplWorkerMagic[] = "LUNA_REPL_WORKER_V7";
constexpr char ReplWorkerRequestMagic[] = "LUNA_REPL_REQUEST_V1";

} // namespace

std::string serializeWorkerRequest(llvm::StringRef operation, llvm::StringRef virtualPath,
                                   llvm::StringRef source) {
    std::ostringstream output;
    output << ReplWorkerRequestMagic << '\n'
           << operation.str() << '\n'
           << virtualPath.size() << '\n'
           << source.size() << '\n';
    output.write(virtualPath.data(), static_cast<std::streamsize>(virtualPath.size()));
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output.str();
}

bool parseWorkerRequest(llvm::StringRef request, std::string& operation, std::string& virtualPath,
                        std::string& source) {
    std::istringstream input(request.str());
    std::string magic;
    std::string virtualPathSizeLine;
    std::string sourceSizeLine;
    if (!std::getline(input, magic) || !std::getline(input, operation) ||
        !std::getline(input, virtualPathSizeLine) || !std::getline(input, sourceSizeLine) ||
        magic != ReplWorkerRequestMagic)
        return false;
    uint64_t virtualPathSize = 0;
    uint64_t sourceSize = 0;
    const auto parseSize = [](const std::string& line, uint64_t& value) {
        const auto result = std::from_chars(line.data(), line.data() + line.size(), value);
        return !line.empty() && result.ec == std::errc{} && result.ptr == line.data() + line.size();
    };
    if (!parseSize(virtualPathSizeLine, virtualPathSize) ||
        !parseSize(sourceSizeLine, sourceSize) || virtualPathSize > MaxReplCellBytes ||
        sourceSize > MaxReplWorkerSourceBytes ||
        (operation != "run" && operation != "validate" && operation != "type"))
        return false;
    virtualPath.resize(static_cast<size_t>(virtualPathSize));
    source.resize(static_cast<size_t>(sourceSize));
    const auto readExact = [&](std::string& value) {
        if (value.empty()) return true;
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
        return input.gcount() == static_cast<std::streamsize>(value.size());
    };
    return readExact(virtualPath) && readExact(source) &&
           input.peek() == std::char_traits<char>::eof();
}

std::string serializeWorkerResult(ReplWorkerStatus status, int exitCode,
                                  const ReplWorkerTimings& timings, llvm::StringRef error) {
    constexpr llvm::StringLiteral TruncationMarker = "\n[worker result truncated]";
    std::string boundedError = error.str();
    if (boundedError.size() > MaxReplWorkerErrorBytes) {
        boundedError.resize(MaxReplWorkerErrorBytes - TruncationMarker.size());
        boundedError += TruncationMarker.str();
    }
    std::ostringstream output;
    output << ReplWorkerMagic << '\n'
           << static_cast<unsigned>(status) << '\n'
           << exitCode << '\n'
           << timings.requestMicroseconds << '\n'
           << timings.linkLoadMicroseconds << '\n'
           << timings.frontendMicroseconds << '\n'
           << timings.lexerMicroseconds << '\n'
           << timings.parserMicroseconds << '\n'
           << timings.semanticMicroseconds << '\n'
           << timings.traitMicroseconds << '\n'
           << timings.ownershipMicroseconds << '\n'
           << timings.indexingMicroseconds << '\n'
           << timings.loweringMicroseconds << '\n'
           << timings.verificationMicroseconds << '\n'
           << timings.sealingMicroseconds << '\n'
           << timings.optimizationMicroseconds << '\n'
           << timings.codegenMicroseconds << '\n'
           << timings.codegenTotalMicroseconds << '\n'
           << timings.jitMaterializationMicroseconds << '\n'
           << timings.jitLookupMicroseconds << '\n'
           << timings.executionMicroseconds << '\n'
           << timings.jitCleanupMicroseconds << '\n'
           << timings.jitTotalMicroseconds << '\n'
           << timings.runningPublishMicroseconds << '\n'
           << timings.workerTotalMicroseconds << '\n'
           << boundedError.size() << '\n';
    output.write(boundedError.data(), static_cast<std::streamsize>(boundedError.size()));
    return output.str();
}

WorkerResultParseResult parseWorkerResult(llvm::StringRef input, size_t& offset,
                                          ReplWorkerStatus& status, int& exitCode,
                                          ReplWorkerTimings& timings, std::string& error) {
    size_t cursor = offset;
    const auto readLine = [&](llvm::StringRef& line) {
        const size_t newline = input.find('\n', cursor);
        if (newline == llvm::StringRef::npos) return false;
        line = input.slice(cursor, newline);
        cursor = newline + 1;
        return true;
    };
    llvm::StringRef magic;
    llvm::StringRef statusLine;
    llvm::StringRef exitCodeLine;
    std::array<llvm::StringRef, 22> timingLines;
    llvm::StringRef errorSizeLine;
    if (!readLine(magic)) return WorkerResultParseResult::Incomplete;
    if (magic != ReplWorkerMagic) return WorkerResultParseResult::Invalid;
    if (!readLine(statusLine) || !readLine(exitCodeLine))
        return WorkerResultParseResult::Incomplete;
    for (auto& line : timingLines)
        if (!readLine(line)) return WorkerResultParseResult::Incomplete;
    if (!readLine(errorSizeLine)) return WorkerResultParseResult::Incomplete;

    unsigned statusValue = 0;
    const auto statusResult = std::from_chars(statusLine.begin(), statusLine.end(), statusValue);
    if (statusResult.ec != std::errc{} || statusResult.ptr != statusLine.end() ||
        statusValue > static_cast<unsigned>(ReplWorkerStatus::Running))
        return WorkerResultParseResult::Invalid;

    int parsedExitCode = 0;
    const auto exitResult =
        std::from_chars(exitCodeLine.begin(), exitCodeLine.end(), parsedExitCode);
    if (exitResult.ec != std::errc{} || exitResult.ptr != exitCodeLine.end())
        return WorkerResultParseResult::Invalid;

    const auto parseUnsigned = [](llvm::StringRef line, uint64_t& value) {
        const auto result = std::from_chars(line.begin(), line.end(), value);
        return !line.empty() && result.ec == std::errc{} && result.ptr == line.end();
    };
    ReplWorkerTimings parsedTimings;
    const std::array<uint64_t*, 22> timingFields = {
        &parsedTimings.requestMicroseconds,        &parsedTimings.linkLoadMicroseconds,
        &parsedTimings.frontendMicroseconds,       &parsedTimings.lexerMicroseconds,
        &parsedTimings.parserMicroseconds,         &parsedTimings.semanticMicroseconds,
        &parsedTimings.traitMicroseconds,          &parsedTimings.ownershipMicroseconds,
        &parsedTimings.indexingMicroseconds,       &parsedTimings.loweringMicroseconds,
        &parsedTimings.verificationMicroseconds,   &parsedTimings.sealingMicroseconds,
        &parsedTimings.optimizationMicroseconds,   &parsedTimings.codegenMicroseconds,
        &parsedTimings.codegenTotalMicroseconds,   &parsedTimings.jitMaterializationMicroseconds,
        &parsedTimings.jitLookupMicroseconds,      &parsedTimings.executionMicroseconds,
        &parsedTimings.jitCleanupMicroseconds,     &parsedTimings.jitTotalMicroseconds,
        &parsedTimings.runningPublishMicroseconds, &parsedTimings.workerTotalMicroseconds,
    };
    for (size_t index = 0; index < timingFields.size(); ++index)
        if (!parseUnsigned(timingLines[index], *timingFields[index]))
            return WorkerResultParseResult::Invalid;
    uint64_t errorSize = 0;
    if (!parseUnsigned(errorSizeLine, errorSize) || errorSize > MaxReplWorkerErrorBytes)
        return WorkerResultParseResult::Invalid;
    if (errorSize > input.size() - cursor) return WorkerResultParseResult::Incomplete;

    status = static_cast<ReplWorkerStatus>(statusValue);
    exitCode = parsedExitCode;
    timings = parsedTimings;
    error = input.substr(cursor, static_cast<size_t>(errorSize)).str();
    offset = cursor + static_cast<size_t>(errorSize);
    return WorkerResultParseResult::Parsed;
}

} // namespace luna::driver::repl_detail
