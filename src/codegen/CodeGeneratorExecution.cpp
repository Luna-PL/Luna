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

#ifdef _WIN32
void lunaJitMingwMain();
#endif

llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> materializeLunaJit(
    std::unique_ptr<llvm::Module>& module,
    std::unique_ptr<llvm::LLVMContext>& context) {
    using namespace llvm;
    using namespace llvm::orc;

    auto jit = LLJITBuilder().create();
    if (!jit) return jit.takeError();

    SymbolMap runtimeSymbols;
    const auto exported = JITSymbolFlags::Exported;
    auto bindRuntime = [&](StringRef name, auto* address) {
        if (!module->getFunction(name)) return;
        runtimeSymbols[(*jit)->mangleAndIntern(name)] =
            ExecutorSymbolDef::fromPtr(address, exported);
    };
    bindRuntime("rt_alloc", &rt_alloc);
    bindRuntime("rt_realloc", &rt_realloc);
    bindRuntime("rt_dealloc", &rt_dealloc);
    bindRuntime("rt_rc_allocate_v1", &rt_rc_allocate_v1);
    bindRuntime("rt_rc_retain_v1", &rt_rc_retain_v1);
    bindRuntime("rt_rc_release_v1", &rt_rc_release_v1);
    bindRuntime("rt_arc_allocate_v1", &rt_arc_allocate_v1);
    bindRuntime("rt_arc_retain_v1", &rt_arc_retain_v1);
    bindRuntime("rt_arc_release_v1", &rt_arc_release_v1);
    bindRuntime("rt_panic_cstr", &rt_panic_cstr);
    bindRuntime("rt_host_services_v1", &rt_host_services_v1);
    bindRuntime("rt_install_application_host_services_v1",
                &rt_install_application_host_services_v1);
    bindRuntime("rt_checked_array_layout_v1", &rt_checked_array_layout_v1);
    bindRuntime("rt_try_alloc_v1", &rt_try_alloc_v1);
    bindRuntime("rt_try_realloc_v1", &rt_try_realloc_v1);
    bindRuntime("rt_console_write_v1", &rt_console_write_v1);
    bindRuntime("rt_console_flush_v1", &rt_console_flush_v1);
    bindRuntime("rt_console_read_v1", &rt_console_read_v1);
    bindRuntime("rt_file_open_v1", &rt_file_open_v1);
    bindRuntime("rt_file_read_v1", &rt_file_read_v1);
    bindRuntime("rt_file_write_v1", &rt_file_write_v1);
    bindRuntime("rt_file_seek_v1", &rt_file_seek_v1);
    bindRuntime("rt_file_flush_v1", &rt_file_flush_v1);
    bindRuntime("rt_file_sync_v1", &rt_file_sync_v1);
    bindRuntime("rt_file_close_v1", &rt_file_close_v1);
    bindRuntime("rt_file_metadata_v1", &rt_file_metadata_v1);
    bindRuntime("rt_path_metadata_v1", &rt_path_metadata_v1);
    bindRuntime("rt_remove_file_v1", &rt_remove_file_v1);
    bindRuntime("rt_create_directory_v1", &rt_create_directory_v1);
    bindRuntime("rt_runtime_error_snapshot_v1", &rt_runtime_error_snapshot_v1);
    bindRuntime("rt_malloc", &rt_malloc);
    bindRuntime("rt_free", &rt_free);
    bindRuntime("rt_print_i32", &rt_print_i32);
    bindRuntime("rt_print_cstr", &rt_print_cstr);
    bindRuntime("rt_compat_console_write_cstr_0_2",
                &rt_compat_console_write_cstr_0_2);
    bindRuntime("rt_compat_console_write_i32_0_2",
                &rt_compat_console_write_i32_0_2);
    bindRuntime("rt_compat_console_flush_0_2",
                &rt_compat_console_flush_0_2);
    bindRuntime("rt_compat_console_read_line_lossy_0_2",
                &rt_compat_console_read_line_lossy_0_2);
    bindRuntime("rt_compat_parse_i32_or_0_2",
                &rt_compat_parse_i32_or_0_2);
    bindRuntime("rt_array_index_or_abort", &rt_array_index_or_abort);
    bindRuntime("rt_dynamic_fragment_select", &rt_dynamic_fragment_select);
    bindRuntime("rt_dynamic_fragment_matches", &rt_dynamic_fragment_matches);
    bindRuntime("rt_dynamic_fragment_report_unknown_and_abort",
                &rt_dynamic_fragment_report_unknown_and_abort);
    bindRuntime("rt_fragment_plugin_load", &rt_fragment_plugin_load);
    bindRuntime("rt_fragment_plugin_last_error", &rt_fragment_plugin_last_error);
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
    runtimeSymbols[(*jit)->mangleAndIntern("__main")] =
        ExecutorSymbolDef::fromPtr(&lunaJitMingwMain, exported);
#endif
    if (!runtimeSymbols.empty()) {
        if (auto error = (*jit)->getMainJITDylib().define(
                absoluteSymbols(std::move(runtimeSymbols))))
            return error;
    }

    auto tsm = ThreadSafeModule(std::move(module), std::move(context));
    if (auto error = (*jit)->addIRModule(std::move(tsm)))
        return error;

    auto& executionSession = (*jit)->getExecutionSession();
    auto processSymbols =
        EPCDynamicLibrarySearchGenerator::GetForTargetProcess(executionSession);
    if (!processSymbols) return processSymbols.takeError();
    (*jit)->getMainJITDylib().addGenerator(std::move(*processSymbols));
    return std::move(*jit);
}

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

struct LunaJitModule::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

LunaJitModule::LunaJitModule() : mImpl(std::make_unique<Impl>()) {}
LunaJitModule::~LunaJitModule() = default;

const void* LunaJitModule::lookup(
    const std::string& symbol, std::string& error) const {
    if (!mImpl || !mImpl->jit) {
        error = "JIT module is not materialized";
        return nullptr;
    }
    auto address = mImpl->jit->lookup(symbol);
    if (!address) {
        error = llvm::toString(address.takeError());
        return nullptr;
    }
    error.clear();
    return address->toPtr<void*>();
}

CodeGenerator::CodeGenerator(const std::string& moduleName)
    : mCtx(std::make_unique<llvm::LLVMContext>())
    , mModule(std::make_unique<llvm::Module>(moduleName, *mCtx))
    , mBuilder(std::make_unique<llvm::IRBuilder<>>(*mCtx))
    , mHelpers(std::make_unique<CGHelpers>(*mCtx)) {
    initializeLLVM();
}

CodeGenerator::~CodeGenerator() = default;

int CodeGenerator::jitRun() {
    std::string error;
    auto module = materializeJitModule(error);
    if (!module) {
        llvm::errs() << "JIT: " << error << "\n";
        return 1;
    }
    const auto address = module->lookup("main", error);
    if (!address) {
        llvm::errs() << "JIT: " << error << "\n";
        return 1;
    }
    auto mainFunction = reinterpret_cast<LunaJitEntry>(
        const_cast<void*>(address));
    return invokeLunaJitEntry(mainFunction);
}

std::shared_ptr<LunaJitModule> CodeGenerator::materializeJitModule(
    std::string& error) {
    if (!mModule || !mCtx) {
        error = "LLVM module was already consumed by JIT materialization";
        return {};
    }
    auto jit = materializeLunaJit(mModule, mCtx);
    if (!jit) {
        error = llvm::toString(jit.takeError());
        return {};
    }
    auto result = std::shared_ptr<LunaJitModule>(new LunaJitModule());
    result->mImpl->jit = std::move(*jit);
    error.clear();
    return result;
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
