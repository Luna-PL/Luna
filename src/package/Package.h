#pragma once

#include "../parser/AST.h"
#include <memory>
#include <string>
#include <vector>

struct PackageExport {
    std::string name;
    std::string kind;
};

struct LoadedPackage {
    std::unique_ptr<Program> program;
    std::string rootPath;
    std::vector<std::string> sourceFiles;
};

// Loads either one .luna source file or every .luna file in a directory.
// Directory sources are merged into one Program before semantic analysis.
class PackageLoader {
public:
    static bool load(const std::string& path, LoadedPackage& result,
                     std::vector<std::string>& errors);

    // Returns the package's explicit public surface. Declarations not marked
    // with `export` are intentionally absent from this interface.
    static std::vector<PackageExport> exports(const Program* program);
};
