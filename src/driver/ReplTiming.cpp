#include "driver/ReplTiming.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <utility>

namespace luna::driver::repl_detail {
namespace {

uint64_t elapsedMicroseconds(ReplTimingReporter::TimePoint start,
                             ReplTimingReporter::TimePoint end) {
    const auto measured =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    return measured > 0 ? static_cast<uint64_t>(measured) : 0;
}

void printDuration(std::ostream& output, const char* name,
                   const std::optional<ReplTimingReporter::TimePoint>& start,
                   const std::optional<ReplTimingReporter::TimePoint>& end) {
    output << name << '=';
    if (start && end)
        output << elapsedMicroseconds(*start, *end) << "us ";
    else
        output << "unavailable ";
}

template <size_t Count>
uint64_t unaccountedDuration(uint64_t total, const std::array<uint64_t, Count>& durations) {
    for (const uint64_t duration : durations)
        total = total > duration ? total - duration : 0;
    return total;
}

} // namespace

ReplTimingReporter::ReplTimingReporter(bool enabled, std::string operation, std::ostream& errors)
    : mEnabled(enabled), mOperation(std::move(operation)), mErrors(errors), mStart(Clock::now()) {}

ReplTimingReporter::~ReplTimingReporter() {
    if (!mEnabled) return;
    const auto end = Clock::now();
    const uint64_t total = elapsedMicroseconds(mStart, end);
    mErrors << "timing[repl]: operation=" << mOperation << " cache=" << (mCacheHit ? "hit" : "miss")
            << " prewarmed=" << (mCacheHit ? "unused" : (mWorkerPrewarmed ? "yes" : "no")) << ' ';
    if (mHasWorkerTimings) {
        mErrors << "frontend=" << mWorkerTimings.frontendMicroseconds << "us "
                << "lexer=" << mWorkerTimings.lexerMicroseconds << "us "
                << "parser=" << mWorkerTimings.parserMicroseconds << "us "
                << "semantic=" << mWorkerTimings.semanticMicroseconds << "us "
                << "traits=" << mWorkerTimings.traitMicroseconds << "us "
                << "ownership=" << mWorkerTimings.ownershipMicroseconds << "us "
                << "indexing=" << mWorkerTimings.indexingMicroseconds << "us "
                << "lowering=" << mWorkerTimings.loweringMicroseconds << "us "
                << "verification=" << mWorkerTimings.verificationMicroseconds << "us "
                << "sealing=" << mWorkerTimings.sealingMicroseconds << "us "
                << "moon-opt=" << mWorkerTimings.optimizationMicroseconds << "us "
                << "codegen=" << mWorkerTimings.codegenMicroseconds << "us "
                << "codegen-setup="
                << (mWorkerTimings.codegenTotalMicroseconds > mWorkerTimings.codegenMicroseconds
                        ? mWorkerTimings.codegenTotalMicroseconds -
                              mWorkerTimings.codegenMicroseconds
                        : 0)
                << "us codegen-total=" << mWorkerTimings.codegenTotalMicroseconds << "us "
                << "jit-materialize=" << mWorkerTimings.jitMaterializationMicroseconds << "us "
                << "jit-lookup=" << mWorkerTimings.jitLookupMicroseconds << "us "
                << "execution=" << mWorkerTimings.executionMicroseconds << "us "
                << "jit-cleanup=" << mWorkerTimings.jitCleanupMicroseconds << "us "
                << "jit-total=" << mWorkerTimings.jitTotalMicroseconds << "us "
                << "worker-request=" << mWorkerTimings.requestMicroseconds << "us "
                << "worker-link=" << mWorkerTimings.linkLoadMicroseconds << "us "
                << "worker-running-publish=" << mWorkerTimings.runningPublishMicroseconds << "us "
                << "worker-other="
                << unaccountedDuration(mWorkerTimings.workerTotalMicroseconds,
                                       std::array<uint64_t, 6>{
                                           mWorkerTimings.requestMicroseconds,
                                           mWorkerTimings.linkLoadMicroseconds,
                                           mWorkerTimings.frontendMicroseconds,
                                           mWorkerTimings.codegenTotalMicroseconds,
                                           mWorkerTimings.jitTotalMicroseconds,
                                           mWorkerTimings.runningPublishMicroseconds,
                                       })
                << "us worker-total=" << mWorkerTimings.workerTotalMicroseconds << "us ";
    } else {
        mErrors << "frontend=unavailable lexer=unavailable parser=unavailable "
                   "semantic=unavailable traits=unavailable ownership=unavailable "
                   "indexing=unavailable lowering=unavailable verification=unavailable "
                   "sealing=unavailable moon-opt=unavailable codegen=unavailable "
                   "codegen-setup=unavailable codegen-total=unavailable "
                   "jit-materialize=unavailable jit-lookup=unavailable "
                   "execution=unavailable jit-cleanup=unavailable "
                   "jit-total=unavailable worker-request=unavailable "
                   "worker-link=unavailable worker-running-publish=unavailable "
                   "worker-other=unavailable worker-total=unavailable ";
    }

    if (mCacheHit) {
        mErrors << "parent-cache=" << total
                << "us parent-acquire=0us parent-submit=0us parent-roundtrip=0us "
                   "parent-roundtrip-gap=0us parent-collect=0us parent-replenish=0us ";
    } else {
        mErrors << "parent-cache=0us ";
        printDuration(mErrors, "parent-acquire", std::optional<TimePoint>{mStart}, mWorkerAcquired);
        printDuration(mErrors, "parent-submit", mWorkerAcquired, mRequestSubmitted);
        printDuration(mErrors, "parent-roundtrip", mRequestSubmitted, mWorkerFinished);
        mErrors << "parent-roundtrip-gap=";
        if (mRequestSubmitted && mWorkerFinished && mHasWorkerTimings) {
            const uint64_t roundtrip = elapsedMicroseconds(*mRequestSubmitted, *mWorkerFinished);
            const uint64_t gap = roundtrip > mWorkerTimings.workerTotalMicroseconds
                                     ? roundtrip - mWorkerTimings.workerTotalMicroseconds
                                     : 0;
            mErrors << gap << "us ";
        } else {
            mErrors << "unavailable ";
        }
        printDuration(mErrors, "parent-collect", mWorkerFinished, mResultHandled);
        printDuration(mErrors, "parent-replenish", mResultHandled, std::optional<TimePoint>{end});
    }

    if (mHasWorkerTimings) {
        const uint64_t overhead = unaccountedDuration(
            total, std::array<uint64_t, 1>{mWorkerTimings.workerTotalMicroseconds});
        mErrors << "overhead=" << overhead << "us ";
    } else {
        mErrors << "overhead=unavailable ";
    }
    mErrors << "total=" << total << "us\n";
}

void ReplTimingReporter::setWorkerTimings(const ReplWorkerTimings& timings) {
    mWorkerTimings = timings;
    mHasWorkerTimings = true;
}

void ReplTimingReporter::setWorkerPrewarmed(bool prewarmed) { mWorkerPrewarmed = prewarmed; }

void ReplTimingReporter::setCacheHit() {
    mCacheHit = true;
    mHasWorkerTimings = true;
}

void ReplTimingReporter::mark(std::optional<TimePoint>& destination) {
    if (mEnabled && !destination) destination = Clock::now();
}

void ReplTimingReporter::markWorkerAcquired() { mark(mWorkerAcquired); }
void ReplTimingReporter::markRequestSubmitted() { mark(mRequestSubmitted); }
void ReplTimingReporter::markWorkerFinished() { mark(mWorkerFinished); }
void ReplTimingReporter::markResultHandled() { mark(mResultHandled); }

} // namespace luna::driver::repl_detail
