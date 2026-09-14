#include "driver/Repl.h"

#include "driver/CompilerPipeline.h"
#include "driver/ReplProcess.h"
#include "driver/ReplProtocol.h"
#include "driver/ReplTransport.h"
#include "parser/AST.h"

#include <llvm/Support/DynamicLibrary.h>

#include <chrono>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace luna::driver {
namespace {

using namespace repl_detail;

uint64_t elapsedMicroseconds(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
}

bool readWorkerRequest(uint64_t requestChannel, std::string& operation, std::string& virtualPath,
                       std::string& source) {
    constexpr size_t MaximumRequestBytes = MaxReplWorkerSourceBytes + MaxReplCellBytes + 256;
    std::string request;
    if (!readWorkerChannel(requestChannel, MaximumRequestBytes, request)) return false;
    return parseWorkerRequest(request, operation, virtualPath, source);
}

bool writeWorkerResult(uint64_t resultChannel, ReplWorkerStatus status, int exitCode,
                       const ReplWorkerTimings& timings, const std::string& error) {
    return writeWorkerChannel(resultChannel,
                              serializeWorkerResult(status, exitCode, timings, error));
}

void printPipelineErrors(const CompilerPipeline& pipeline, std::ostream& errors) {
    for (const auto& error : pipeline.errors()) {
        if (!pipeline.errorStage().empty()) errors << "error[" << pipeline.errorStage() << "]: ";
        errors << error << '\n';
    }
}

void copyPipelineTimings(const CompilerPipeline& pipeline, ReplWorkerTimings& timings) {
    const auto& pipelineTimings = pipeline.timings();
    timings.lexerMicroseconds = pipelineTimings.analysis.lexerMicroseconds;
    timings.parserMicroseconds = pipelineTimings.analysis.parserMicroseconds;
    timings.semanticMicroseconds = pipelineTimings.analysis.semanticMicroseconds;
    timings.traitMicroseconds = pipelineTimings.analysis.traitMicroseconds;
    timings.ownershipMicroseconds = pipelineTimings.analysis.ownershipMicroseconds;
    timings.indexingMicroseconds = pipelineTimings.analysis.indexingMicroseconds;
    timings.loweringMicroseconds = pipelineTimings.loweringMicroseconds;
    timings.verificationMicroseconds = pipelineTimings.verificationMicroseconds;
    timings.sealingMicroseconds = pipelineTimings.sealingMicroseconds;
    timings.optimizationMicroseconds = pipelineTimings.optimizationMicroseconds;
    timings.codegenMicroseconds = pipelineTimings.codegenMicroseconds;
}

} // namespace

int runReplWorker(std::ostream& errors, const std::vector<std::string>& linkLibraries,
                  uint64_t requestChannel, uint64_t resultChannel, uint64_t readySignal,
                  uint64_t gateSignal, uint64_t completionSignal, uint64_t stdoutChannel,
                  uint64_t stderrChannel, unsigned parentProcessId, unsigned memoryLimitMiB,
                  LunaOptimizationLevel optimizationLevel) {
    ReplWorkerTimings timings;
    std::optional<std::chrono::steady_clock::time_point> workerStart;
    const auto finish = [&](ReplWorkerStatus status, int exitCode,
                            const std::string& error = std::string()) {
        if (workerStart) timings.workerTotalMicroseconds = elapsedMicroseconds(*workerStart);
        closeWorkerOutput();
        if (!writeWorkerResult(resultChannel, status, exitCode, timings, error)) {
            errors << "error[repl-worker]: cannot write result channel\n";
            closeWorkerChannel(resultChannel);
            return 2;
        }
        closeWorkerChannel(resultChannel);
        if (!publishWorkerSignal(completionSignal, "completion", errors)) {
            errors << "error[repl-worker]: cannot publish completion\n";
            return 2;
        }
        return 0;
    };

    if (!protectWorkerHandles(requestChannel, resultChannel, stdoutChannel, stderrChannel,
                              readySignal, gateSignal, completionSignal, errors))
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "failed to protect worker channels from descendants");

    if (!redirectWorkerOutput(stdoutChannel, stderrChannel, errors))
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "failed to redirect worker output channels");

    if (!applyWorkerMemoryLimit(memoryLimitMiB, errors))
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "failed to apply worker memory limit");

    // Populate only LLVM's immutable target registries before readiness. The
    // worker remains single-shot and does not retain compiler, JIT, linked
    // library, or user runtime state across cells.
    initializeLunaLLVMTargets();

    if (!waitForWorkerGate(readySignal, gateSignal, parentProcessId, errors))
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "failed to establish process containment");

    workerStart = std::chrono::steady_clock::now();
    std::string operation;
    std::string virtualPath;
    std::string source;
    const auto requestStart = std::chrono::steady_clock::now();
    const bool requestSucceeded = readWorkerRequest(requestChannel, operation, virtualPath, source);
    timings.requestMicroseconds = elapsedMicroseconds(requestStart);
    if (!requestSucceeded)
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "cannot read or validate worker request");

    const auto linkLoadStart = std::chrono::steady_clock::now();
    if (operation == "run") {
        for (const auto& library : linkLibraries) {
            std::string loadError;
            if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(library.c_str(), &loadError)) {
                timings.linkLoadMicroseconds = elapsedMicroseconds(linkLoadStart);
                errors << "error[repl-worker]: cannot load JIT library '" << library
                       << "': " << loadError << '\n';
                return finish(ReplWorkerStatus::InfrastructureError, 0,
                              "cannot load JIT library '" + library + "'");
            }
        }
    }
    timings.linkLoadMicroseconds = elapsedMicroseconds(linkLoadStart);

    CompilerPipeline pipeline;
    CompilerPipelineOptions options;
    options.optimizationLevel = optimizationLevel;
    const auto frontendStart = std::chrono::steady_clock::now();
    const bool frontendSucceeded = pipeline.compileSourceToMoonIR(source, virtualPath, options);
    timings.frontendMicroseconds = elapsedMicroseconds(frontendStart);
    copyPipelineTimings(pipeline, timings);
    if (!frontendSucceeded) {
        printPipelineErrors(pipeline, errors);
        return finish(ReplWorkerStatus::Rejected, 0);
    }
    if (operation == "type") {
        constexpr char ProbeName[] = "__luna_repl_type_probe";
        const auto* program = pipeline.analysisSnapshot().program();
        for (const auto& declaration : program->declarations) {
            const auto* function = dynamic_cast<const FunctionDecl*>(declaration.get());
            if (!function || function->name != "main" || !function->body) continue;
            for (const auto& statement : function->body->stmts) {
                const auto* binding = dynamic_cast<const LetStmt*>(statement.get());
                if (binding && binding->name == ProbeName && binding->inferredType)
                    return finish(ReplWorkerStatus::Executed, 0, binding->inferredType->toString());
            }
        }
        return finish(ReplWorkerStatus::InfrastructureError, 0,
                      "compiler did not retain the type probe");
    }
    const auto codegenTotalStart = std::chrono::steady_clock::now();
    const bool codegenSucceeded = pipeline.generateCode({});
    timings.codegenTotalMicroseconds = elapsedMicroseconds(codegenTotalStart);
    copyPipelineTimings(pipeline, timings);
    if (!codegenSucceeded) {
        printPipelineErrors(pipeline, errors);
        return finish(ReplWorkerStatus::Rejected, 0);
    }
    if (operation == "validate") return finish(ReplWorkerStatus::Executed, 0);
    const auto runningPublishStart = std::chrono::steady_clock::now();
    const bool runningPublished =
        writeWorkerResult(resultChannel, ReplWorkerStatus::Running, 0, timings, {});
    timings.runningPublishMicroseconds = elapsedMicroseconds(runningPublishStart);
    if (!runningPublished) {
        errors << "error[repl-worker]: cannot write running state\n";
        closeWorkerChannel(resultChannel);
        return 2;
    }
    const auto jitStart = std::chrono::steady_clock::now();
    const auto execution = pipeline.codeGenerator().jitRun();
    timings.jitTotalMicroseconds = elapsedMicroseconds(jitStart);
    timings.jitMaterializationMicroseconds = execution.materializationMicroseconds;
    timings.jitLookupMicroseconds = execution.lookupMicroseconds;
    timings.executionMicroseconds = execution.executionMicroseconds;
    timings.jitCleanupMicroseconds = execution.cleanupMicroseconds;
    if (!execution.executed)
        return finish(ReplWorkerStatus::InfrastructureError, 0, execution.error);
    return finish(ReplWorkerStatus::Executed, execution.exitCode);
}

} // namespace luna::driver
