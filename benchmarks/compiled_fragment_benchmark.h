#pragma once

namespace luna::benchmarks {
// Reuses the compiler test harness's frontend/backend linkage. Timing is
// opt-in; the default regression calls only the untimed correctness gate.
int runCompiledFragmentBenchmark(int argc, char** argv);
int checkCompiledFragmentWorkload();
} // namespace luna::benchmarks
