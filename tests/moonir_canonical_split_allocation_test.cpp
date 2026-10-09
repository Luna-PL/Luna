#include "moonir_canonical_test_support.h"

#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "runtime/Runtime.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace canonical_test {
namespace {

struct AllocationRecord {
    void* pointer = nullptr;
    size_t size = 0;
    size_t alignment = 0;
    bool released = false;
};

struct AllocationProbe {
    bool tracking = false;
    std::vector<AllocationRecord> records;
    size_t releases = 0;
    size_t unmatchedReleases = 0;
    size_t layoutMismatches = 0;
    size_t reallocations = 0;
};

int caseFail(const char* name, const char* message) {
    std::cerr << name << ": " << message << '\n';
    return 1;
}

void* probeAllocate(void* context, size_t size, size_t alignment) {
    auto& probe = *static_cast<AllocationProbe*>(context);
    void* pointer = std::malloc(size);
    if (probe.tracking && pointer)
        probe.records.push_back({pointer, size, alignment, false});
    return pointer;
}

void* probeReallocate(
    void* context, void* pointer, size_t, size_t newSize, size_t alignment) {
    auto& probe = *static_cast<AllocationProbe*>(context);
    void* replacement = std::realloc(pointer, newSize);
    if (probe.tracking) {
        ++probe.reallocations;
        if (replacement) {
            for (auto& record : probe.records) {
                if (record.pointer == pointer && !record.released) {
                    record.pointer = replacement;
                    record.size = newSize;
                    record.alignment = alignment;
                    break;
                }
            }
        }
    }
    return replacement;
}

void probeDeallocate(
    void* context, void* pointer, size_t size, size_t alignment) {
    auto& probe = *static_cast<AllocationProbe*>(context);
    if (probe.tracking) {
        bool found = false;
        for (auto& record : probe.records) {
            if (record.pointer != pointer || record.released) continue;
            record.released = true;
            if (record.size != size || record.alignment != alignment)
                ++probe.layoutMismatches;
            ++probe.releases;
            found = true;
            break;
        }
        if (!found) {
            ++probe.unmatchedReleases;
            return;
        }
    }
    std::free(pointer);
}

int runSourceCase(AllocationProbe& probe, const char* name,
                  const std::string& source, int expectedExitCode,
                  size_t expectedCells = 2, size_t expectedOuters = 1) {
    luna::driver::CompilerPipeline pipeline;
    if (!pipeline.compileSourceToMoonIR(source, name) ||
        !pipeline.generateCode({})) {
        for (const auto& diagnostic : pipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return caseFail(name, "did not compile");
    }

    probe.records.clear();
    probe.releases = 0;
    probe.unmatchedReleases = 0;
    probe.layoutMismatches = 0;
    probe.reallocations = 0;
    probe.tracking = true;
    const auto execution = pipeline.codeGenerator().jitRun();
    probe.tracking = false;
    if (!execution.executed || execution.exitCode != expectedExitCode) {
        std::cerr << execution.error << '\n';
        return caseFail(name, "did not execute");
    }
    const size_t expectedAllocations = expectedCells + expectedOuters;
    if (probe.records.size() != expectedAllocations ||
        probe.releases != expectedAllocations ||
        probe.unmatchedReleases != 0 || probe.layoutMismatches != 0 ||
        probe.reallocations != 0)
        return caseFail(name, "did not release each field and outer allocation once");
    size_t cells = 0;
    size_t outers = 0;
    for (const auto& record : probe.records) {
        if (!record.released) {
            return caseFail(name, "retained an allocation after return");
        }
        if (record.size == 4 && record.alignment == 4)
            ++cells;
        else if (record.size == 16 && record.alignment == 8)
            ++outers;
    }
    if (cells != expectedCells || outers != expectedOuters)
        return caseFail(name, "did not preserve the field/outer allocation layout");
    return 0;
}

int runCase(AllocationProbe& probe, const char* name, const char* body) {
    const std::string source = std::string(R"luna(
struct Cell { marker: i32; }
impl Drop for Cell {
    fn drop(resource: &mut Cell) -> unit { resource.marker = 0; }
}
struct Pair { first: Cell; second: Cell; }
fn main() -> i32 {
    let pair = new Pair(move new Cell(41), move new Cell(43));
)luna") + body + "\n}\n";
    return runSourceCase(probe, name, source, 0);
}

int runFromCase(AllocationProbe& probe, const char* name,
                int firstMarker, int expectedExitCode) {
    const std::string source = std::string(R"luna(
struct Cell { marker: i32; }
impl Drop for Cell {
    fn drop(resource: &mut Cell) -> unit { resource.marker = 0; }
}
struct SourceSplitError { first: Cell; second: Cell; }
impl From<SourceSplitError> for i32 {
    fn from(affine error: SourceSplitError) -> i32 {
        if error.first.marker > 0 {
            let forwarded = move error.first;
            return forwarded.marker;
        }
        let forwarded = move error.second;
        return forwarded.marker;
    }
}
fn converted() -> Result<i32, i32> {
)luna") + "    let first = new Cell(" + std::to_string(firstMarker) + R"luna();
    let second = new Cell(47);
    let error = new SourceSplitError(move first, move second);
    let input = Err::<i32, SourceSplitError>(move error);
    let value = input?;
    return Ok(value);
}
fn main() -> i32 {
    let result = converted();
    return unwrap_err(move result);
}
)luna";
    return runSourceCase(probe, name, source, expectedExitCode);
}

int runFromMergeCase(AllocationProbe& probe, const char* name,
                     int firstMarker) {
    const std::string source = std::string(R"luna(
struct Cell { marker: i32; }
impl Drop for Cell {
    fn drop(resource: &mut Cell) -> unit { resource.marker = 0; }
}
struct SourceMergeError { first: Cell; second: Cell; }
impl From<SourceMergeError> for i32 {
    fn from(affine error: SourceMergeError) -> i32 {
        if error.first.marker > 0 {
            let forwarded = move error.first;
        } else {
            let forwarded = move error.first;
        }
        return error.second.marker;
    }
}
fn converted() -> Result<i32, i32> {
)luna") + "    let first = new Cell(" + std::to_string(firstMarker) + R"luna();
    let second = new Cell(47);
    let error = new SourceMergeError(move first, move second);
    let input = Err::<i32, SourceMergeError>(move error);
    let value = input?;
    return Ok(value);
}
fn main() -> i32 {
    let result = converted();
    return unwrap_err(move result);
}
)luna";
    return runSourceCase(probe, name, source, 47);
}

int runFromEarlyContinueCase(AllocationProbe& probe, const char* name,
                              int firstMarker) {
    const std::string source = std::string(R"luna(
struct Cell { marker: i32; }
impl Drop for Cell {
    fn drop(resource: &mut Cell) -> unit { resource.marker = 0; }
}
struct SourceEarlyContinueError { first: Cell; second: Cell; }
impl From<SourceEarlyContinueError> for i32 {
    fn from(affine error: SourceEarlyContinueError) -> i32 {
        if error.first.marker > 0 {
            let forwarded = move error.first;
            return forwarded.marker;
        } else {
            let forwarded = move error.second;
        }
        return 0 - error.first.marker;
    }
}
fn converted() -> Result<i32, i32> {
)luna") + "    let first = new Cell(" + std::to_string(firstMarker) + R"luna();
    let second = new Cell(47);
    let error = new SourceEarlyContinueError(move first, move second);
    let input = Err::<i32, SourceEarlyContinueError>(move error);
    let value = input?;
    return Ok(value);
}
fn main() -> i32 {
    let result = converted();
    return unwrap_err(move result);
}
)luna";
    return runSourceCase(probe, name, source, 43);
}

int runFromOwnedResultCase(AllocationProbe& probe) {
    const std::string source = R"luna(
struct SourceError { marker: i32; }
impl Drop for SourceError {
    fn drop(resource: &mut SourceError) -> unit { resource.marker = 0; }
}
struct ReturnedResource { marker: i32; }
impl Drop for ReturnedResource {
    fn drop(resource: &mut ReturnedResource) -> unit { resource.marker = 0; }
}
impl From<SourceError> for ReturnedResource {
    fn from(affine error: SourceError) -> ReturnedResource {
        let returned = new ReturnedResource(error.marker + 12);
        return move returned;
    }
}
fn converted() -> Result<i32, ReturnedResource> {
    let source = new SourceError(47);
    let input = Err::<i32, SourceError>(move source);
    let value = input?;
    return Ok(value);
}
fn main() -> i32 {
    let result = converted();
    let returned = unwrap_err(move result);
    return returned.marker;
}
)luna";
    return runSourceCase(probe, "<from-owned-result>", source, 59, 2, 0);
}

int runFromSplitOwnedResultCase(AllocationProbe& probe) {
    const std::string source = R"luna(
struct Cell { marker: i32; }
impl Drop for Cell {
    fn drop(resource: &mut Cell) -> unit { resource.marker = 0; }
}
struct SourceSplitError { first: Cell; second: Cell; }
struct ReturnedFromSplit { marker: i32; inner: Cell; }
impl Drop for ReturnedFromSplit {
    fn drop(resource: &mut ReturnedFromSplit) -> unit { resource.marker = 0; }
}
impl From<SourceSplitError> for ReturnedFromSplit {
    fn from(affine error: SourceSplitError) -> ReturnedFromSplit {
        let carried = move error.first;
        let returned = new ReturnedFromSplit(59, move carried);
        return move returned;
    }
}
fn converted() -> Result<i32, ReturnedFromSplit> {
    let first = new Cell(43);
    let second = new Cell(47);
    let error = new SourceSplitError(move first, move second);
    let input = Err::<i32, SourceSplitError>(move error);
    let value = input?;
    return Ok(value);
}
fn main() -> i32 {
    let result = converted();
    let returned = unwrap_err(move result);
    return returned.marker;
}
)luna";
    return runSourceCase(probe, "<split-from-owned-result>", source, 59, 2, 2);
}

} // namespace

int runSplitAllocationProbe() {
    AllocationProbe probe;
    const LunaAllocatorV1 allocator{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaAllocatorV1), &probe,
        probeAllocate, probeReallocate, probeDeallocate};
    const LunaHostServicesV1 services{
        LUNA_HOST_SERVICES_MAGIC_V1, LUNA_RUNTIME_ABI_V1,
        LUNA_HOST_SERVICES_V1_BASE_SIZE, 0, LUNA_HOST_CAP_ALLOCATOR,
        &allocator, nullptr, nullptr, nullptr};
    if (rt_install_host_services_v1(&services) != LUNA_RUNTIME_STATUS_OK)
        return fail("split allocation probe could not install its allocator");
    if (runCase(probe, "<split-one-field>",
                "let first = move pair.first; return 0;"))
        return 1;
    if (runCase(probe, "<split-all-fields>",
                "let first = move pair.first;\n"
                "let second = move pair.second; return 0;"))
        return 1;
    if (runCase(probe, "<split-branch-merge-true>",
                "if true { let first = move pair.first; }\n"
                "else { let first = move pair.first; } return 0;"))
        return 1;
    if (runCase(probe, "<split-branch-merge-false>",
                "if false { let first = move pair.first; }\n"
                "else { let first = move pair.first; } return 0;"))
        return 1;
    if (runCase(probe, "<split-branch-returns-first>",
                "if true { let first = move pair.first; return 0; }\n"
                "else { let second = move pair.second; return 0; }"))
        return 1;
    if (runCase(probe, "<split-branch-returns-second>",
                "if false { let first = move pair.first; return 0; }\n"
                "else { let second = move pair.second; return 0; }"))
        return 1;
    if (runCase(probe, "<split-branch-early-return>",
                "if true { let first = move pair.first; return 0; }\n"
                "else { let second = move pair.second; } return 0;"))
        return 1;
    if (runCase(probe, "<split-branch-continues>",
                "if false { let first = move pair.first; return 0; }\n"
                "else { let second = move pair.second; } return 0;"))
        return 1;
    if (runFromCase(probe, "<split-from-first>", 43, 43)) return 1;
    if (runFromCase(probe, "<split-from-second>", -43, 47)) return 1;
    if (runFromMergeCase(probe, "<split-from-merge-first>", 43)) return 1;
    if (runFromMergeCase(probe, "<split-from-merge-second>", -43)) return 1;
    if (runFromEarlyContinueCase(probe, "<split-from-early-return>", 43))
        return 1;
    if (runFromEarlyContinueCase(probe, "<split-from-continue>", -43))
        return 1;
    if (runFromOwnedResultCase(probe)) return 1;
    if (runFromSplitOwnedResultCase(probe)) return 1;
    return 0;
}

} // namespace canonical_test
