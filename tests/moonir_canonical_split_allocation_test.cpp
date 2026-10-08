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
    if (!execution.executed || execution.exitCode != 0) {
        std::cerr << execution.error << '\n';
        return caseFail(name, "did not execute");
    }
    if (probe.records.size() != 3 || probe.releases != 3 ||
        probe.unmatchedReleases != 0 || probe.layoutMismatches != 0 ||
        probe.reallocations != 0)
        return caseFail(name, "did not release each Cell and the Pair allocation once");
    size_t cells = 0;
    size_t pairs = 0;
    for (const auto& record : probe.records) {
        if (!record.released) {
            return caseFail(name, "retained an allocation after return");
        }
        if (record.size == 4 && record.alignment == 4)
            ++cells;
        else if (record.size == 16 && record.alignment == 8)
            ++pairs;
    }
    if (cells != 2 || pairs != 1)
        return caseFail(name, "did not preserve the Cell/Pair allocation layout");
    return 0;
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
    return 0;
}

} // namespace canonical_test
