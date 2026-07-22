#include "PackageManager.h"

#include "../diagnostics/Diagnostic.h"
#include "../lexer/Lexer.h"
#include "../parser/Parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool readSource(const fs::path& path, std::string& source,
                std::vector<std::string>& errors) {
    std::ifstream file(path);
    if (!file) {
        errors.push_back(diagnostic::format(
            "package", "cannot read source file", path.string(), 0, 0,
            "check that the path exists and is readable"));
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    source = buffer.str();
    return true;
}

bool parseSource(const fs::path& path,
                 const luna::macro::MacroProcessor& macroProcessor,
                 std::unique_ptr<Program>& program,
                 std::vector<std::string>& errors) {
    std::string source;
    if (!readSource(path, source, errors)) return false;

    luna::macro::Expansion expansion;
    if (!macroProcessor.process({path.string(), std::move(source)}, expansion, errors))
        return false;

    Lexer lexer(expansion.source, path.string());
    auto tokens = lexer.tokenize();
    errors.insert(errors.end(), lexer.errors().begin(), lexer.errors().end());
    if (!lexer.errors().empty()) return false;

    Parser parser(std::move(tokens), path.string(), expansion.source);
    program = parser.parse();
    errors.insert(errors.end(), parser.errors().begin(), parser.errors().end());
    return parser.errors().empty();
}

} // namespace

PackageManager::PackageManager(luna::macro::MacroProcessor macroProcessor)
    : mMacroProcessor(std::move(macroProcessor)) {}

bool PackageManager::load(const PackageRequest& request, LoadedPackage& result,
                          PackageGraph& graph,
                          std::vector<std::string>& errors) const {
    result = {};
    graph = {};
    std::error_code ec;
    fs::path input(request.inputPath);
    if (!fs::exists(input, ec)) {
        errors.push_back(diagnostic::format(
            "package", "input path does not exist: '" + request.inputPath + "'",
            request.inputPath, 0, 0,
            "pass a .luna file or a package directory"));
        return false;
    }

    std::vector<fs::path> files;
    const bool isDirectory = fs::is_directory(input, ec);
    if (isDirectory) {
        graph.rootPath = fs::absolute(input, ec).string();
        for (const auto& entry : fs::directory_iterator(input, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec) && entry.path().extension() == ".luna")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            errors.push_back(diagnostic::format(
                "package", "package directory contains no .luna files",
                request.inputPath, 0, 0,
                "add at least one file with the .luna extension"));
            return false;
        }
    } else {
        files.push_back(input);
        graph.rootPath = fs::absolute(input.parent_path(), ec).string();
    }

    result.rootPath = graph.rootPath;
    result.program = std::make_unique<Program>();
    result.program->isPackage = isDirectory;

    bool success = true;
    for (const auto& file : files) {
        std::unique_ptr<Program> sourceProgram;
        if (!parseSource(file, mMacroProcessor, sourceProgram, errors)) {
            success = false;
            continue;
        }
        graph.sourceUnits.push_back(file.string());
        result.sourceFiles.push_back(file.string());
        result.program->sourceFiles.push_back(file.string());

        if (!sourceProgram->packageName.empty()) {
            if (result.program->packageName.empty()) {
                result.program->packageName = sourceProgram->packageName;
            } else if (result.program->packageName != sourceProgram->packageName) {
                errors.push_back(diagnostic::format(
                    "package", "package name '" + sourceProgram->packageName +
                    "' does not match '" + result.program->packageName + "'",
                    file.string(), 0, 0,
                    "all files in a package must use the same `package` declaration"));
                success = false;
            }
        }

        for (auto& declaration : sourceProgram->declarations)
            result.program->declarations.push_back(std::move(declaration));
    }

    if (result.program->isPackage && result.program->packageName.empty())
        result.program->packageName = input.filename().string();

    return success && errors.empty();
}
