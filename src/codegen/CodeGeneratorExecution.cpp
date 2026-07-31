#include "CodeGenerator.h"

#include "../runtime/Runtime.h"

#include <llvm/Support/Compiler.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include <memory>
#include <utility>

namespace {

void initializeLLVM() {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        initialized = true;
    }
}

using LunaJitEntry = int (*)();

// UBSan's `function` check probes metadata immediately before every indirect
// call target. ORC-generated functions do not carry that compiler-emitted
// metadata and may begin at a page boundary, where the probe itself would
// fault. Keep the single JIT boundary exempt while retaining ASan/UBSan
// instrumentation everywhere else in the compiler and embedded runtime.
#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
int invokeLunaJitEntry(LunaJitEntry entry) {
    return entry();
}

#ifdef _WIN32
// MinGW inserts a call to __main when lowering a function named `main` so a
// native executable can run GCC-style global constructors. Luna JIT modules
// contain no such CRT constructor tables, while the statically linked MinGW
// implementation is not exported for ORC process lookup. This no-op provides
// exactly the compiler-inserted support symbol required to materialize the JIT
// entry point without depending on the host executable's export table.
void lunaJitMingwMain() {}
#endif

} // namespace

CodeGenerator::CodeGenerator(const std::string& moduleName)
    : mCtx(std::make_unique<llvm::LLVMContext>())
    , mModule(std::make_unique<llvm::Module>(moduleName, *mCtx))
    , mBuilder(std::make_unique<llvm::IRBuilder<>>(*mCtx))
    , mHelpers(std::make_unique<CGHelpers>(*mCtx)) {
    initializeLLVM();
}

CodeGenerator::~CodeGenerator() = default;

int CodeGenerator::jitRun() {
    using namespace llvm;
    using namespace llvm::orc;

    auto JIT = LLJITBuilder().create();
    if (!JIT) {
        llvm::errs() << "JIT: " << toString(JIT.takeError()) << "\n";
        return 1;
    }

    // Luna runtime calls are language ABI symbols, not ambient process
    // symbols. Register every referenced helper explicitly so JIT behavior
    // does not depend on ELF -rdynamic, Mach-O export policy, or Windows
    // __declspec(dllexport). System/user-library symbols remain available via
    // the target-process search generator below.
    SymbolMap runtimeSymbols;
    const auto exported = JITSymbolFlags::Exported;
    auto bindRuntime = [&](StringRef name, auto* address) {
        if (!mModule->getFunction(name)) return;
        runtimeSymbols[(*JIT)->mangleAndIntern(name)] =
            ExecutorSymbolDef::fromPtr(address, exported);
    };
    bindRuntime("rt_alloc", &rt_alloc);
    bindRuntime("rt_realloc", &rt_realloc);
    bindRuntime("rt_dealloc", &rt_dealloc);
    bindRuntime("rt_rc_alloc", &rt_rc_alloc);
    bindRuntime("rt_rc_retain", &rt_rc_retain);
    bindRuntime("rt_rc_release", &rt_rc_release);
    bindRuntime("rt_arc_alloc", &rt_arc_alloc);
    bindRuntime("rt_arc_retain", &rt_arc_retain);
    bindRuntime("rt_arc_release", &rt_arc_release);
    bindRuntime("rt_shared_dealloc", &rt_shared_dealloc);
    bindRuntime("rt_panic_cstr", &rt_panic_cstr);
    bindRuntime("rt_host_services_v1", &rt_host_services_v1);
    bindRuntime("rt_runtime_error_snapshot_v1",
                &rt_runtime_error_snapshot_v1);
    bindRuntime("rt_malloc", &rt_malloc);
    bindRuntime("rt_free", &rt_free);
    bindRuntime("rt_print_i32", &rt_print_i32);
    bindRuntime("rt_print_cstr", &rt_print_cstr);
    bindRuntime("rt_array_index_or_abort", &rt_array_index_or_abort);
    bindRuntime("rt_dynamic_fragment_select", &rt_dynamic_fragment_select);
    bindRuntime("rt_dynamic_fragment_matches", &rt_dynamic_fragment_matches);
    bindRuntime("rt_dynamic_fragment_report_unknown_and_abort",
                &rt_dynamic_fragment_report_unknown_and_abort);
    bindRuntime("rt_fragment_plugin_load", &rt_fragment_plugin_load);
    bindRuntime("rt_fragment_plugin_last_error",
                &rt_fragment_plugin_last_error);
    bindRuntime("rt_fragment_plugin_is_registered",
                &rt_fragment_plugin_is_registered);
    bindRuntime("rt_fragment_plugin_invoke", &rt_fragment_plugin_invoke);
    bindRuntime("rt_fragment_plugin_report_error_and_abort",
                &rt_fragment_plugin_report_error_and_abort);
    bindRuntime("rt_gpu_initialize", &rt_gpu_initialize);
    bindRuntime("rt_gpu_backend_name", &rt_gpu_backend_name);
    bindRuntime("rt_gpu_last_error", &rt_gpu_last_error);
    bindRuntime("rt_gpu_report_initialization_error",
                &rt_gpu_report_initialization_error);
    bindRuntime("rt_gpu_report_operation_error_and_abort",
                &rt_gpu_report_operation_error_and_abort);
    bindRuntime("rt_gpu_backend_is_cuda", &rt_gpu_backend_is_cuda);
    bindRuntime("rt_gpu_backend_is_rocm", &rt_gpu_backend_is_rocm);
    bindRuntime("rt_gpu_alloc_i32", &rt_gpu_alloc_i32);
    bindRuntime("rt_gpu_free", &rt_gpu_free);
    bindRuntime("rt_gpu_load_i32", &rt_gpu_load_i32);
    bindRuntime("rt_gpu_store_i32", &rt_gpu_store_i32);
    bindRuntime("rt_gpu_copy_from_host_i32", &rt_gpu_copy_from_host_i32);
    bindRuntime("rt_gpu_copy_to_host_i32", &rt_gpu_copy_to_host_i32);
    bindRuntime("rt_gpu_launch_ptx", &rt_gpu_launch_ptx);
    bindRuntime("rt_gpu_launch_hsaco", &rt_gpu_launch_hsaco);
    bindRuntime("rt_gpu_await_event", &rt_gpu_await_event);
#ifdef _WIN32
    runtimeSymbols[(*JIT)->mangleAndIntern("__main")] =
        ExecutorSymbolDef::fromPtr(&lunaJitMingwMain, exported);
#endif
    if (!runtimeSymbols.empty()) {
        if (auto err = (*JIT)->getMainJITDylib().define(
                absoluteSymbols(std::move(runtimeSymbols)))) {
            llvm::errs() << "JIT runtime symbols: "
                         << toString(std::move(err)) << "\n";
            return 1;
        }
    }

    auto tsm = ThreadSafeModule(std::move(mModule), std::move(mCtx));
    auto err = (*JIT)->addIRModule(std::move(tsm));
    if (err) {
        llvm::errs() << "JIT: " << toString(std::move(err)) << "\n";
        return 1;
    }

    // Resolve libc and explicitly loaded user-library symbols. Luna's own
    // runtime symbols were defined above and never rely on this fallback.
    auto& executionSession = (*JIT)->getExecutionSession();
    auto processSymbols =
        EPCDynamicLibrarySearchGenerator::GetForTargetProcess(executionSession);
    if (processSymbols)
        (*JIT)->getMainJITDylib().addGenerator(std::move(*processSymbols));

    auto mainSymbol = (*JIT)->lookup("main");
    if (!mainSymbol) {
        llvm::errs() << "JIT: " << toString(mainSymbol.takeError()) << "\n";
        return 1;
    }

    auto mainFunction = mainSymbol->toPtr<int()>();
    return invokeLunaJitEntry(mainFunction);
}

bool CodeGenerator::emitObjectFile(const std::string& outputPath) {
    auto targetTriple = llvm::sys::getProcessTriple();
    mModule->setTargetTriple(llvm::Triple(targetTriple));

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error("Cannot open output file: " + ec.message());
        return false;
    }

    mModule->print(dest, nullptr); // text IR, avoids bitcode compat issues
    dest.flush();
    return true;
}
