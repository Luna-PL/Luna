#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "parser/AST.h"
#include "sema/TypeSystem.h"
#include "sema/SymbolTable.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/TraitChecker.h"
#include "sema/OwnershipChecker.h"
#include "moonir/Lowering.h"
#include "moonir/Verifier.h"
#include "moonir/Optimizer.h"
#include "moonir/Printer.h"
#include "codegen/CodeGenerator.h"
#include "runtime/Runtime.h"
#include "Version.h"
#include "package/Package.h"
#include "diagnostics/Diagnostic.h"
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Program.h>

#include <iostream>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

#ifndef LUNA_DL_LIBRARY
#define LUNA_DL_LIBRARY ""
#endif

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Error: Cannot read file '" << path << "'\n";
        std::exit(1);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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
  • Algebraic data types (struct Point { x: i32; }, enum Option<T> { ... })
  • Packages (a directory of .luna files; use `export` for public declarations)
  • Nominal ADT binding with structural fields and layouts
  • Ownership system (move, borrow, auto-free)
  • Linear values and strict shared/mutable borrow checking (linear, borrow mut, &mut)
  • Heterogeneous compute surface (kernel fn, device_buffer<T>, launch, await)
  • GPU code targets via --gpu-target=sim|cuda[:sm_*]|rocm[:gfx*]
  • Runtime backend via LUNA_GPU_BACKEND=sim|cuda|rocm (default: sim)

Examples: see examples/*.luna
)";
}

static std::string quoteForDisplay(const std::string& value) {
    if (value.find_first_of(" \t\"'") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
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

static bool isLibraryPath(const std::string& value) {
    auto endsWith = [&value](const char* suffix) {
        const std::string suffixString(suffix);
        return value.size() >= suffixString.size() &&
               value.compare(value.size() - suffixString.size(), suffixString.size(), suffixString) == 0;
    };
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos ||
           endsWith(".a") || endsWith(".so") || endsWith(".dylib") ||
           endsWith(".dll") || endsWith(".lib");
}

static bool parseGpuTargets(const std::string& specification,
                            LunaGpuTargetConfig& targets,
                            std::string& error) {
    if (specification.empty()) {
        error = "GPU target list must not be empty";
        return false;
    }
    size_t start = 0;
    while (start <= specification.size()) {
        const size_t comma = specification.find(',', start);
        const std::string item = specification.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item.empty()) {
            error = "GPU target list contains an empty target";
            return false;
        }
        const size_t colon = item.find(':');
        const std::string backend = item.substr(0, colon);
        const std::string architecture = colon == std::string::npos
            ? "" : item.substr(colon + 1);
        if (backend == "sim") {
            if (colon != std::string::npos) {
                error = "sim GPU target does not accept an architecture";
                return false;
            }
        } else if (backend == "cuda") {
            const std::string selected = architecture.empty() ? "sm_52" : architecture;
            if (colon != std::string::npos && architecture.empty()) {
                error = "CUDA GPU target requires an architecture after ':'";
                return false;
            }
            if (selected.rfind("sm_", 0) != 0) {
                error = "CUDA architecture must use the sm_* spelling";
                return false;
            }
            if (targets.emitPTX && targets.cudaArchitecture != selected) {
                error = "one artifact cannot contain multiple CUDA architectures yet";
                return false;
            }
            targets.emitPTX = true;
            targets.cudaArchitecture = selected;
        } else if (backend == "rocm") {
            const std::string selected = architecture.empty() ? "gfx1101" : architecture;
            if (colon != std::string::npos && architecture.empty()) {
                error = "ROCm GPU target requires an architecture after ':'";
                return false;
            }
            if (selected.rfind("gfx", 0) != 0) {
                error = "ROCm architecture must use the gfx* spelling";
                return false;
            }
            if (targets.emitHSACO && targets.rocmArchitecture != selected) {
                error = "one artifact cannot contain multiple ROCm architectures yet";
                return false;
            }
            targets.emitHSACO = true;
            targets.rocmArchitecture = selected;
        } else {
            error = "unknown GPU target '" + backend +
                "'; expected sim, cuda[:sm_*], or rocm[:gfx*]";
            return false;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

int main(int argc, char* argv[]) {
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
        std::cout << "REPL mode — JIT-compiling expressions one at a time\n";
        std::cout << "Type 'exit' to quit.\n\n";

        std::string line;
        while (true) {
            std::cout << "luna> ";
            if (!std::getline(std::cin, line) || line == "exit") break;

            if (line.empty()) continue;

            // Wrap line into a minimal program
            std::string source = "fn main() -> i32 {\n    " + line + ";\n    return 0;\n}";

            Lexer lexer(source, "<repl>");
            auto tokens = lexer.tokenize();

            if (!lexer.errors().empty()) {
                printErrors(lexer.errors());
                continue;
            }

            Parser parser(std::move(tokens), "<repl>", source);
            auto prog = parser.parse();

            if (!parser.errors().empty()) {
                printErrors(parser.errors());
                continue;
            }

            SemanticAnalyzer sema;
            if (!sema.analyze(prog.get())) {
                printErrors(sema.errors());
                continue;
            }

            TraitChecker traits;
            if (!traits.check(prog.get())) {
                printErrors(traits.errors());
                continue;
            }

            OwnershipChecker owner;
            if (!owner.check(prog.get(), sema.symTable())) {
                printErrors(owner.errors());
                continue;
            }

            moon::LunaLowerer lowerer;
            auto moonModule = lowerer.lower(*prog, sema.symTable());
            if (!lowerer.errors().empty()) {
                printErrors(lowerer.errors(), "moon-lower");
                continue;
            }
            moon::Verifier verifier;
            if (!verifier.verify(*moonModule)) {
                printErrors(verifier.errors(), "moon-verify");
                continue;
            }
            moon::Optimizer optimizer;
            if (!optimizer.run(*moonModule, {
                    moon::OptimizationLevel::None,
                    moon::OptimizationPurpose::JustInTime})) {
                printErrors(optimizer.errors(), "moon-opt");
                continue;
            }
            if (!verifier.verify(*moonModule)) {
                printErrors(verifier.errors(), "moon-verify");
                continue;
            }

            CodeGenerator cg("repl");
            if (!cg.generate(moonModule.get())) {
                printErrors(cg.errors());
                continue;
            }

            int result = cg.jitRun();
            // Generated `print` calls use C stdio. On Windows a redirected
            // stdout is block-buffered independently from the C++ stream, so
            // establish the user-output-before-prompt ordering explicitly.
            std::fflush(stdout);
            std::cout << "= " << result << "\n";
        }
        return 0;
    }

    if (argc < 3) {
        std::cerr << "Error: Missing file argument\n";
        printUsage();
        return 1;
    }

    std::string filePath = argv[2];
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string aotCompiler;
    std::string moonIrOutput;
    LunaGpuTargetConfig gpuTargets;
    bool printMoonCostReport = false;
    bool reserveKernelRuntime = false;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
    for (int i = 3; i < argc; ++i) {
        std::string option = argv[i];
        auto parseOptimizationLevel = [&optimizationLevel](const std::string& value) {
            if (value == "-O0" || value == "O0") optimizationLevel = LunaOptimizationLevel::O0;
            else if (value == "-O2" || value == "O2") optimizationLevel = LunaOptimizationLevel::O2;
            else if (value == "-O3" || value == "O3") optimizationLevel = LunaOptimizationLevel::O3;
            else return false;
            return true;
        };
        if (parseOptimizationLevel(option)) {
            continue;
        } else if (option == "--opt" && i + 1 < argc) {
            if (!parseOptimizationLevel(argv[++i])) {
                std::cerr << "Unsupported optimization level: " << argv[i] << "\n";
                return 1;
            }
        } else if (option.rfind("--opt=", 0) == 0) {
            if (!parseOptimizationLevel(option.substr(6))) {
                std::cerr << "Unsupported optimization level: " << option.substr(6) << "\n";
                return 1;
            }
        } else if (option == "--link" && i + 1 < argc) {
            linkLibraries.push_back(argv[++i]);
        } else if (option.rfind("--link=", 0) == 0) {
            linkLibraries.push_back(option.substr(7));
        } else if (option == "--runtime-lib" && i + 1 < argc) {
            runtimeLibrary = argv[++i];
        } else if (option.rfind("--runtime-lib=", 0) == 0) {
            runtimeLibrary = option.substr(14);
        } else if (option == "--cc" && i + 1 < argc) {
            aotCompiler = argv[++i];
        } else if (option.rfind("--cc=", 0) == 0) {
            aotCompiler = option.substr(5);
        } else if (option == "--gpu-target" && i + 1 < argc) {
            std::string targetError;
            if (!parseGpuTargets(argv[++i], gpuTargets, targetError)) {
                std::cerr << "Invalid --gpu-target: " << targetError << "\n";
                return 1;
            }
        } else if (option.rfind("--gpu-target=", 0) == 0) {
            std::string targetError;
            if (!parseGpuTargets(option.substr(13), gpuTargets, targetError)) {
                std::cerr << "Invalid --gpu-target: " << targetError << "\n";
                return 1;
            }
        } else if (option == "--reserve-kernel-runtime") {
            reserveKernelRuntime = true;
        } else if (option == "--moon-cost-report") {
            printMoonCostReport = true;
        } else if (option == "--emit-moonir" && i + 1 < argc) {
            moonIrOutput = argv[++i];
        } else if (option.rfind("--emit-moonir=", 0) == 0) {
            moonIrOutput = option.substr(14);
        } else {
            std::cerr << "Unknown option: " << option << "\n";
            printUsage();
            return 1;
        }
    }

    if (cmd != "build" && (!runtimeLibrary.empty() || !aotCompiler.empty())) {
        std::cerr << diagnostic::format(
            "driver", "AOT linker options are only valid with `build`",
            "", 0, 0, "use `luna build ... --runtime-lib <path> --cc <compiler>") << "\n";
        return 1;
    }

    // ─── Pipeline ──────────────────────────────────────────────────
    // 1-2. Load and parse one source file or an entire package directory.
    LoadedPackage loaded;
    std::vector<std::string> packageErrors;
    if (!PackageLoader::load(filePath, loaded, packageErrors)) {
        printErrors(packageErrors);
        return 1;
    }
    auto* prog = loaded.program.get();

    // 3. Semantic Analysis (type checking + auto inference)
    SemanticAnalyzer sema;
    if (!sema.analyze(prog)) {
        printErrors(sema.errors());
        return 1;
    }

    // 4. Trait/Constraint checking
    TraitChecker traits;
    if (!traits.check(prog)) {
        printErrors(traits.errors());
        return 1;
    }

    // 5. Ownership checking
    OwnershipChecker owner;
    if (!owner.check(prog, sema.symTable())) {
        printErrors(owner.errors());
        return 1;
    }

    // 6. Lower the checked Luna program into the sole backend input.
    moon::LunaLowerer lowerer;
    auto moonModule = lowerer.lower(*prog, sema.symTable(), reserveKernelRuntime);
    if (!lowerer.errors().empty()) {
        printErrors(lowerer.errors(), "moon-lower");
        return 1;
    }

    // 7. MoonIR is a security and correctness boundary. Both AOT and JIT
    // reject invalid modules before an LLVMContext is allowed to consume them.
    moon::Verifier verifier;
    if (!verifier.verify(*moonModule)) {
        printErrors(verifier.errors(), "moon-verify");
        return 1;
    }

    moon::OptimizationLevel moonOptimizationLevel = moon::OptimizationLevel::None;
    if (optimizationLevel == LunaOptimizationLevel::O2)
        moonOptimizationLevel = moon::OptimizationLevel::Standard;
    else if (optimizationLevel == LunaOptimizationLevel::O3)
        moonOptimizationLevel = moon::OptimizationLevel::Aggressive;
    moon::Optimizer optimizer;
    if (!optimizer.run(*moonModule, {
            moonOptimizationLevel,
            cmd == "build" ? moon::OptimizationPurpose::AheadOfTime
                           : moon::OptimizationPurpose::JustInTime})) {
        printErrors(optimizer.errors(), "moon-opt");
        return 1;
    }
    if (!verifier.verify(*moonModule)) {
        printErrors(verifier.errors(), "moon-verify");
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
        moonPrinter.print(*moonModule, output);
    }
    if (printMoonCostReport) moonPrinter.printCostReport(*moonModule, std::cout);

    // 8. LLVM lowering consumes MoonIR only.
    CodeGenerator cg(prog->packageName.empty() ? filePath : prog->packageName);
    cg.setOptimizationLevel(optimizationLevel);
    cg.setGpuTargets(std::move(gpuTargets));
    if (!cg.generate(moonModule.get())) {
        printErrors(cg.errors());
        return 1;
    }

    if (cmd == "run") {
        if (!loadJITLibraries(linkLibraries)) return 1;
        int result = cg.jitRun();
        // Keep the CLI marker after all observable program output on every
        // CRT, including MinGW/UCRT under redirected GitHub Actions stdout.
        std::fflush(stdout);
        std::cout << "Program exited with code: " << result << std::endl;
        return result;
    }

    if (cmd == "build") {
        namespace fs = std::filesystem;
        fs::path inputPath(filePath);
        fs::path irPath;
        fs::path exePath;
        if (fs::is_directory(inputPath)) {
            const std::string packageName = prog->packageName.empty()
                ? inputPath.filename().string() : prog->packageName;
            irPath = inputPath / (packageName + ".ll");
            exePath = inputPath / packageName;
        } else {
            irPath = inputPath.string() + ".ll";
            exePath = inputPath.parent_path() /
                inputPath.stem();
        }
#ifdef _WIN32
        exePath += ".exe";
#endif

        std::cout << "Emitting LLVM IR: " << irPath.string() << "\n";
        if (!cg.emitObjectFile(irPath.string())) {
            printErrors(cg.errors());
            return 1;
        }

        // AOT remains self-contained when run from the build tree, but an
        // installed driver can supply its runtime and compiler explicitly or
        // through environment variables.  This makes packaging reproducible
        // without silently linking against an unrelated build directory.
        if (runtimeLibrary.empty()) {
            if (const char* configured = std::getenv("LUNA_RUNTIME_LIB"))
                runtimeLibrary = configured;
        }
        if (runtimeLibrary.empty()) runtimeLibrary = std::string(BUILD_DIR) + "/libruntime.a";
        if (!fs::exists(runtimeLibrary)) {
            std::cerr << diagnostic::format(
                "driver", "runtime library does not exist: '" + runtimeLibrary + "'",
                runtimeLibrary, 0, 0,
                "pass `--runtime-lib <path>` or set LUNA_RUNTIME_LIB to Luna's libruntime.a") << "\n";
            return 1;
        }
        if (aotCompiler.empty()) {
            if (const char* configured = std::getenv("LUNA_CXX")) aotCompiler = configured;
        }
        if (aotCompiler.empty()) aotCompiler = "clang++";

        const char* aotOptimizationFlag = "-O0";
        if (optimizationLevel == LunaOptimizationLevel::O2)
            aotOptimizationFlag = "-O2";
        else if (optimizationLevel == LunaOptimizationLevel::O3)
            aotOptimizationFlag = "-O3";
        auto compilerPath = llvm::sys::findProgramByName(aotCompiler);
        if (!compilerPath) {
            std::cerr << diagnostic::format(
                "driver", "cannot find AOT compiler '" + aotCompiler + "': " +
                    compilerPath.getError().message(),
                aotCompiler, 0, 0,
                "pass --cc with an executable path or add the compiler to PATH") << "\n";
            return 1;
        }

        std::vector<std::string> linkerArgs = {
            *compilerPath,
            aotOptimizationFlag,
            irPath.generic_string(),
            runtimeLibrary,
        };
        if (std::string(LUNA_DL_LIBRARY).size())
            linkerArgs.push_back("-l" + std::string(LUNA_DL_LIBRARY));
        linkerArgs.push_back("-o");
        linkerArgs.push_back(exePath.generic_string());
        for (const auto& library : linkLibraries) {
            linkerArgs.push_back(isLibraryPath(library) ? library : "-l" + library);
        }

        std::cout << "Linking:";
        for (const auto& argument : linkerArgs)
            std::cout << ' ' << quoteForDisplay(argument);
        std::cout << "\n";

        std::vector<llvm::StringRef> linkerArgRefs;
        linkerArgRefs.reserve(linkerArgs.size());
        for (const auto& argument : linkerArgs) linkerArgRefs.emplace_back(argument);
        std::string executionError;
        const int linkResult = llvm::sys::ExecuteAndWait(
            *compilerPath, linkerArgRefs, std::nullopt, {}, 0, 0, &executionError);
        if (linkResult != 0) {
            std::cerr << diagnostic::format(
                "driver", "AOT linker '" + aotCompiler + "' failed with status " +
                std::to_string(linkResult) +
                    (executionError.empty() ? "" : ": " + executionError), "", 0, 0,
                "inspect the linker command above; verify --cc, --runtime-lib, and every --link dependency") << "\n";
            return 1;
        }

        std::cout << "Built executable: " << exePath.string() << "\n";
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    printUsage();
    return 1;
}
