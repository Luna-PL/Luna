#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "parser/AST.h"
#include "sema/TypeSystem.h"
#include "sema/SymbolTable.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/TraitChecker.h"
#include "sema/OwnershipChecker.h"
#include "codegen/CodeGenerator.h"
#include "runtime/Runtime.h"
#include "Version.h"
#include "package/Package.h"
#include "diagnostics/Diagnostic.h"
#include <llvm/Support/DynamicLibrary.h>

#include <iostream>
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
  luna run    <file-or-package> [-O0|-O2|-O3] [--link <shared-library>]  JIT-compile and execute
  luna build  <file-or-package> [-O0|-O2|-O3] [--link <library-or-name>]
                   [--runtime-lib <path>] [--cc <compiler>]  AOT-compile to executable
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
  • GPU backends via LUNA_GPU_BACKEND=sim|cuda|rocm (default: sim)

Examples: see examples/*.luna
)";
}

static std::string shellQuote(const std::string& value) {
#ifdef _WIN32
    // The AOT driver invokes the platform command interpreter through
    // std::system.  Quote according to the Windows C runtime convention and
    // protect backslashes that occur immediately before a quote or at EOL.
    std::string quoted = "\"";
    size_t backslashes = 0;
    for (char c : value) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
        } else {
            quoted.append(backslashes, '\\');
            quoted += c;
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
#else
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
#endif
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
        if (!rt_gpu_initialize()) {
            std::cerr << "GPU backend initialization failed for '" << rt_gpu_backend_name()
                      << "': " << rt_gpu_last_error() << "\n";
            return 1;
        }
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

            CodeGenerator cg("repl");
            if (!cg.generate(prog.get(), &sema.symTable())) {
                printErrors(cg.errors());
                continue;
            }

            int result = cg.jitRun();
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

    if (!rt_gpu_initialize()) {
        std::cerr << "GPU backend initialization failed for '" << rt_gpu_backend_name()
                  << "': " << rt_gpu_last_error() << "\n";
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

    // Generating code
    // 6. Code generation
    CodeGenerator cg(prog->packageName.empty() ? filePath : prog->packageName);
    cg.setOptimizationLevel(optimizationLevel);
    if (!cg.generate(prog, &sema.symTable())) {
        printErrors(cg.errors());
        return 1;
    }

    if (cmd == "run") {
        if (!loadJITLibraries(linkLibraries)) return 1;
        int result = cg.jitRun();
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
        std::string linkerCmd = shellQuote(aotCompiler) + " " +
            shellQuote(aotOptimizationFlag) + " " + shellQuote(irPath.string()) + " " +
            shellQuote(runtimeLibrary);
        if (std::string(LUNA_DL_LIBRARY).size())
            linkerCmd += " " + shellQuote("-l" + std::string(LUNA_DL_LIBRARY));
        linkerCmd += " -o " + shellQuote(exePath.string());
        for (const auto& library : linkLibraries) {
            if (isLibraryPath(library)) linkerCmd += " " + shellQuote(library);
            else linkerCmd += " " + shellQuote("-l" + library);
        }
        std::cout << "Linking: " << linkerCmd << "\n";
        int linkResult = std::system(linkerCmd.c_str());
        if (linkResult != 0) {
            std::cerr << diagnostic::format(
                "driver", "AOT linker '" + aotCompiler + "' failed with status " +
                std::to_string(linkResult), "", 0, 0,
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
