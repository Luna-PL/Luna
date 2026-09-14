#include "CodeGenerator.h"

#include "../runtime/Runtime.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace {

using LunaJitEntry = int (*)();

#ifdef _WIN32
void lunaJitMingwMain();
#endif

llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>>
materializeLunaJit(std::unique_ptr<llvm::Module>& module,
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
    bindRuntime("rt_print_i32", &rt_print_i32);
    bindRuntime("rt_print_u32", &rt_print_u32);
    bindRuntime("rt_print_cstr", &rt_print_cstr);
    bindRuntime("rt_console_write_cstr_v1", &rt_console_write_cstr_v1);
    bindRuntime("rt_console_write_i32_v1", &rt_console_write_i32_v1);
    bindRuntime("rt_console_flush_simple_v1", &rt_console_flush_simple_v1);
    bindRuntime("rt_console_read_line_lossy_v1", &rt_console_read_line_lossy_v1);
    bindRuntime("rt_parse_i32_or_v1", &rt_parse_i32_or_v1);
    bindRuntime("rt_array_index_or_abort", &rt_array_index_or_abort);
    bindRuntime("rt_gpu_initialize", &rt_gpu_initialize);
    bindRuntime("rt_gpu_backend_name", &rt_gpu_backend_name);
    bindRuntime("rt_gpu_last_error", &rt_gpu_last_error);
    bindRuntime("rt_gpu_report_initialization_error", &rt_gpu_report_initialization_error);
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
        if (auto error =
                (*jit)->getMainJITDylib().define(absoluteSymbols(std::move(runtimeSymbols))))
            return error;
    }

    auto tsm = ThreadSafeModule(std::move(module), std::move(context));
    if (auto error = (*jit)->addIRModule(std::move(tsm))) return error;

    auto& executionSession = (*jit)->getExecutionSession();
    auto processSymbols = EPCDynamicLibrarySearchGenerator::GetForTargetProcess(
        executionSession, [](const SymbolStringPtr& symbol) {
            // A missing Luna entry must remain a lookup failure. Mach-O
            // executables export their host `main`, and admitting it here can
            // make jitRun() recursively invoke the compiler/test process.
            const StringRef name = *symbol;
            return name != "main" && name != "_main";
        });
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
int invokeLunaJitEntry(LunaJitEntry entry) { return entry(); }

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

void initializeLunaLLVMTargets() {
    static std::once_flag initialization;
    std::call_once(initialization, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    });
}

namespace {

void initializeLunaLLVMDeviceTargets() {
    static std::once_flag initialization;
    std::call_once(initialization, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
    });
}

} // namespace

struct LunaJitModule::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

LunaJitModule::LunaJitModule() : mImpl(std::make_unique<Impl>()) {}
LunaJitModule::~LunaJitModule() = default;

const void* LunaJitModule::lookup(const std::string& symbol, std::string& error) const {
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
    : mCtx(std::make_unique<llvm::LLVMContext>()),
      mModule(std::make_unique<llvm::Module>(moduleName, *mCtx)),
      mBuilder(std::make_unique<llvm::IRBuilder<>>(*mCtx)),
      mHelpers(std::make_unique<CGHelpers>(*mCtx)) {
    initializeLunaLLVMTargets();
}

CodeGenerator::~CodeGenerator() = default;

void CodeGenerator::setGpuTargets(LunaGpuTargetConfig targets) {
    if (targets.emitPTX || targets.emitHSACO) initializeLunaLLVMDeviceTargets();
    mGpuTargets = std::move(targets);
}

LunaJitRunResult CodeGenerator::jitRun() {
    LunaJitRunResult result;
    std::string error;
    const auto materializationStart = std::chrono::steady_clock::now();
    auto module = materializeJitModule(error);
    result.materializationMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - materializationStart)
                                  .count());
    if (!module) {
        result.error = std::move(error);
        return result;
    }
    const auto lookupStart = std::chrono::steady_clock::now();
    const auto address = module->lookup("main", error);
    result.lookupMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - lookupStart)
                                  .count());
    if (!address) {
        result.error = std::move(error);
        const auto cleanupStart = std::chrono::steady_clock::now();
        module.reset();
        result.cleanupMicroseconds =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - cleanupStart)
                                      .count());
        return result;
    }
    auto mainFunction = reinterpret_cast<LunaJitEntry>(const_cast<void*>(address));
    const auto executionStart = std::chrono::steady_clock::now();
    result.exitCode = invokeLunaJitEntry(mainFunction);
    result.executionMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - executionStart)
                                  .count());
    result.executed = true;
    const auto cleanupStart = std::chrono::steady_clock::now();
    module.reset();
    result.cleanupMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - cleanupStart)
                                  .count());
    return result;
}

std::shared_ptr<LunaJitModule> CodeGenerator::materializeJitModule(std::string& error) {
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
#if LLVM_VERSION_MAJOR >= 22
    mModule->setTargetTriple(llvm::Triple(targetTriple));
#else
    mModule->setTargetTriple(targetTriple);
#endif

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

bool CodeGenerator::emitNativeObjectFile(const std::string& outputPath) {
    initializeLunaLLVMTargets();
    const std::string targetTriple = llvm::sys::getProcessTriple();
    llvm::TargetMachine* machine = mHostTargetMachine.get();
    if (!machine) {
        std::string targetError;
#if LLVM_VERSION_MAJOR >= 22
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
            llvm::Triple(targetTriple), targetError);
        mModule->setTargetTriple(llvm::Triple(targetTriple));
#else
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
            targetTriple, targetError);
        mModule->setTargetTriple(targetTriple);
#endif
        if (!target) {
            error("Cannot select native target: " + targetError);
            return false;
        }

        llvm::TargetOptions targetOptions;
        const auto codegenLevel =
            mOptimizationLevel == LunaOptimizationLevel::O3
                ? llvm::CodeGenOptLevel::Aggressive
                : mOptimizationLevel == LunaOptimizationLevel::O2
                    ? llvm::CodeGenOptLevel::Default
                    : llvm::CodeGenOptLevel::None;
        mHostTargetMachine.reset(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 22
            llvm::Triple(targetTriple), "generic", "", targetOptions,
#else
            targetTriple, "generic", "", targetOptions,
#endif
            llvm::Reloc::PIC_, std::nullopt, codegenLevel));
        if (!mHostTargetMachine) {
            error("Cannot create the native target machine");
            return false;
        }
        machine = mHostTargetMachine.get();
    }
    mModule->setDataLayout(machine->createDataLayout());

    std::error_code fileError;
    llvm::raw_fd_ostream destination(
        outputPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        error("Cannot open native object output: " + fileError.message());
        return false;
    }
    llvm::legacy::PassManager passManager;
    if (machine->addPassesToEmitFile(
            passManager, destination, nullptr,
            llvm::CodeGenFileType::ObjectFile)) {
        error("Native LLVM backend cannot emit an object file");
        return false;
    }
    passManager.run(*mModule);
    destination.flush();
    return true;
}
