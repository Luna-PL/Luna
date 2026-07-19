#include "Package.h"
#include "../lexer/Lexer.h"
#include "../parser/Parser.h"
#include "../diagnostics/Diagnostic.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static std::string publicDeclarationName(const Decl* declaration,
                                         const std::string& sourceName) {
    if (!declaration || !declaration->versionTag) return sourceName;
    return sourceName + "@" + declaration->versionTag->name + "(" +
           declaration->versionTag->version.toString() + ")";
}

static bool readSource(const fs::path& path, std::string& source,
                       std::vector<std::string>& errors) {
    std::ifstream file(path);
    if (!file) {
        errors.push_back(diagnostic::format("package", "cannot read source file", path.string(),
                                            0, 0, "check that the path exists and is readable"));
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    source = buffer.str();
    return true;
}

static bool parseSource(const fs::path& path, std::unique_ptr<Program>& program,
                        std::vector<std::string>& errors) {
    std::string source;
    if (!readSource(path, source, errors)) return false;

    Lexer lexer(source, path.string());
    auto tokens = lexer.tokenize();
    for (const auto& error : lexer.errors())
        errors.push_back(error);
    if (!lexer.errors().empty()) return false;

    Parser parser(std::move(tokens), path.string(), source);
    program = parser.parse();
    for (const auto& error : parser.errors())
        errors.push_back(error);
    return parser.errors().empty();
}

bool PackageLoader::load(const std::string& path, LoadedPackage& result,
                         std::vector<std::string>& errors) {
    result = {};
    std::error_code ec;
    fs::path input(path);
    if (!fs::exists(input, ec)) {
        errors.push_back(diagnostic::format("package", "input path does not exist: '" + path + "'",
                                            path, 0, 0, "pass a .luna file or a package directory"));
        return false;
    }

    std::vector<fs::path> files;
    if (fs::is_directory(input, ec)) {
        result.rootPath = fs::absolute(input, ec).string();
        for (const auto& entry : fs::directory_iterator(input, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec) && entry.path().extension() == ".luna")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            errors.push_back(diagnostic::format("package", "package directory contains no .luna files",
                                                path, 0, 0,
                                                "add at least one file with the .luna extension"));
            return false;
        }
    } else {
        files.push_back(input);
        result.rootPath = fs::absolute(input.parent_path(), ec).string();
    }

    result.program = std::make_unique<Program>();
    result.program->isPackage = fs::is_directory(input, ec);

    bool success = true;
    for (const auto& file : files) {
        std::unique_ptr<Program> sourceProgram;
        if (!parseSource(file, sourceProgram, errors)) {
            success = false;
            continue;
        }
        result.sourceFiles.push_back(file.string());
        result.program->sourceFiles.push_back(file.string());

        if (!sourceProgram->packageName.empty()) {
            if (result.program->packageName.empty()) {
                result.program->packageName = sourceProgram->packageName;
            } else if (result.program->packageName != sourceProgram->packageName) {
                errors.push_back(diagnostic::format(
                    "package", "package name '" + sourceProgram->packageName +
                    "' does not match '" + result.program->packageName + "'", file.string(),
                    0, 0, "all files in a package must use the same `package` declaration"));
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

std::vector<PackageExport> PackageLoader::exports(const Program* program) {
    std::vector<PackageExport> result;
    if (!program) return result;

    for (const auto& declaration : program->declarations) {
        if (!declaration->isExported) continue;
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(function, function->name), "function"});
        else if (auto* fragment = dynamic_cast<FragmentDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(fragment, fragment->name), "fragment"});
        else if (auto* structure = dynamic_cast<StructDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(structure, structure->name), "struct"});
        else if (auto* enumeration = dynamic_cast<EnumDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(enumeration, enumeration->name), "enum"});
        else if (auto* trait = dynamic_cast<TraitDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(trait, trait->name), "trait"});
    }
    return result;
}
