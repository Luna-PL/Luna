#include "driver/Driver.h"
#include "driver/AotLinker.h"
#include "driver/CommandLine.h"
#include "driver/CompilerPipeline.h"
#include "driver/Repl.h"
#include "moonir/Printer.h"
#include "runtime/Runtime.h"
#include "Version.h"
#include "package/Package.h"
#include "diagnostics/Diagnostic.h"
#include <llvm/Support/DynamicLibrary.h>

#include <iostream>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace luna::driver {

static void printErrors(const std::vector<std::string>& errors, const char* stage = nullptr) {
    for (auto& e : errors) {
        if (stage) std::cerr << "error[" << stage << "]: ";
        std::cerr << e << "\n";
    }
}

static void printUsage() {
    std::cout << R"(
╔══════════════════════════════════════════════╗
║           Luna — Luna Language          ║
║   A programming language built with LLVM    ║
╚══════════════════════════════════════════════╝

Usage:
  luna --version            Print the compiler version
  luna check  <file-or-package> [--emit-moonir <path>]  Verify through MoonIR
  luna run    <file-or-package> [-O0|-O2|-O3] [--link <shared-library>]
                   [--gpu-target <target[,target...]>]  JIT-compile and execute
  luna build  <file-or-package> [-O0|-O2|-O3] [--link <library-or-name>]
                   [--runtime-lib <path>] [--cc <compiler>]
                   [--gpu-target <target[,target...]>]
                   [--reserve-kernel-runtime] [--emit-moonir <path>]
                   [--moon-cost-report]  AOT-compile to executable
  luna repl                  Interactive REPL (JIT)

Features:
  • Stack allocation (let x: i32 = 42)
  • Heap allocation (let p = new i32(100))
  • Auto type inference (let z = x + 1)
  • Templates / Generics (fn id<T>(x: T) -> T { x })
  • Trait constraints (fn sum<T: Addable>(a: T, b: T) -> T)
  • Where clauses (where T: Clone)
  • Named compile-time constraints (constraint Small<T> = ...)
  • Iterable static selector views and declaration reflection
  • Algebraic data types (struct Point { x: i32; }, enum Option<T> { ... })
  • Reverse-DNS packages with module/submodule source identities and explicit exports
  • Nominal ADT binding with structural fields and layouts
  • Ownership system (move, borrow, auto-free)
  • Linear values and strict shared/mutable borrow checking (linear, borrow mut, &mut)
  • Heterogeneous compute surface (kernel fn, device_buffer<T>, launch, await)
  • GPU code targets via --gpu-target=sim|cuda[:sm_*]|rocm[:gfx*]
  • Runtime backend via LUNA_GPU_BACKEND=sim|cuda|rocm (default: sim)

Examples: see examples/*.luna
)";
}

static bool loadJITLibraries(const std::vector<std::string>& libraries) {
    for (const auto& library : libraries) {
        std::string error;
        if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(library.c_str(), &error)) {
            std::cerr << "Cannot load JIT library '" << library << "': " << error << "\n";
            return false;
        }
    }
    return true;
}

int run(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
        std::cout << "Luna " << LUNA_VERSION_STRING << "\n";
        return 0;
    }

    if (cmd == "repl") {
        return runRepl(std::cin, std::cout, std::cerr);
    }

    auto parseResult = parseCommandLine(argc, argv);
    if (!parseResult.options) {
        std::cerr << parseResult.error << "\n";
        if (parseResult.showUsage) printUsage();
        return 1;
    }
    auto options = std::move(*parseResult.options);
    auto& filePath = options.inputPath;
    auto& linkLibraries = options.linkLibraries;
    auto& runtimeLibrary = options.runtimeLibrary;
    auto& aotCompiler = options.aotCompiler;
    auto& moonIrOutput = options.moonIrOutput;
    auto& gpuTargets = options.gpuTargets;
    auto& printMoonCostReport = options.printMoonCostReport;
    auto& reserveKernelRuntime = options.reserveKernelRuntime;
    auto& optimizationLevel = options.optimizationLevel;

    if (cmd != "build" && (!runtimeLibrary.empty() || !aotCompiler.empty())) {
        std::cerr << diagnostic::format(
            "driver", "AOT linker options are only valid with `build`",
            "", 0, 0, "use `luna build ... --runtime-lib <path> --cc <compiler>") << "\n";
        return 1;
    }

    CompilerPipeline pipeline;
    if (!pipeline.compileToMoonIR({
            filePath,
            optimizationLevel,
            reserveKernelRuntime,
            cmd == "build"})) {
        printErrors(
            pipeline.errors(),
            pipeline.errorStage().empty()
                ? nullptr : pipeline.errorStage().c_str());
        return 1;
    }

    moon::Printer moonPrinter;
    if (!moonIrOutput.empty()) {
        std::ofstream output(moonIrOutput);
        if (!output) {
            std::cerr << diagnostic::format(
                "driver", "cannot write MoonIR file '" + moonIrOutput + "'",
                moonIrOutput, 0, 0, "check the output directory and permissions") << "\n";
            return 1;
        }
        moonPrinter.print(pipeline.moonModule(), output);
    }
    if (printMoonCostReport)
        moonPrinter.printCostReport(pipeline.moonModule(), std::cout);

    // Library packages deliberately have no main function. `check` validates
    // the complete frontend -> MoonIR boundary without manufacturing an
    // executable entry point or paying LLVM code-generation costs.
    if (cmd == "check") return 0;

    if (!pipeline.generateCode(std::move(gpuTargets))) {
        printErrors(pipeline.errors());
        return 1;
    }
    auto& cg = pipeline.codeGenerator();

    if (cmd == "run") {
        if (!loadJITLibraries(linkLibraries)) return 1;
        int result = cg.jitRun();
        // Keep the CLI marker after all observable program output on every
        // CRT, including MinGW/UCRT under redirected GitHub Actions stdout.
        std::fflush(stdout);
        std::cout << "Program exited with code: " << result << std::endl;
        return result;
    }

    if (cmd == "build")
        return AotLinker::build(cg, {
            filePath,
            pipeline.declaredPackageName(),
            linkLibraries,
            runtimeLibrary,
            aotCompiler,
            optimizationLevel,
        });

    std::cerr << "Unknown command: " << cmd << "\n";
    printUsage();
    return 1;
}

} // namespace luna::driver
