#pragma once

#include "driver/ReplProtocol.h"

#include <chrono>
#include <iosfwd>
#include <optional>
#include <string>

namespace luna::driver::repl_detail {

class ReplTimingReporter {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    ReplTimingReporter(bool enabled, std::string operation, std::ostream& errors);
    ReplTimingReporter(const ReplTimingReporter&) = delete;
    ReplTimingReporter& operator=(const ReplTimingReporter&) = delete;
    ~ReplTimingReporter();

    void setWorkerTimings(const ReplWorkerTimings& timings);
    void setWorkerPrewarmed(bool prewarmed);
    void setCacheHit();
    void markWorkerAcquired();
    void markRequestSubmitted();
    void markWorkerFinished();
    void markResultHandled();

private:
    void mark(std::optional<TimePoint>& destination);

    bool mEnabled = false;
    std::string mOperation;
    std::ostream& mErrors;
    TimePoint mStart;
    ReplWorkerTimings mWorkerTimings;
    std::optional<TimePoint> mWorkerAcquired;
    std::optional<TimePoint> mRequestSubmitted;
    std::optional<TimePoint> mWorkerFinished;
    std::optional<TimePoint> mResultHandled;
    bool mHasWorkerTimings = false;
    bool mWorkerPrewarmed = false;
    bool mCacheHit = false;
};

} // namespace luna::driver::repl_detail
