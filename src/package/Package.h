#pragma once

#include "../parser/AST.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct PackageExport {
    std::string name;
    std::string kind;
    std::string modulePath;
};

struct PackageUse {
    std::string packageId;
    std::string alias;
    std::string sourcePath;
    int line = 0;
    int column = 0;
};

struct PackageManifest {
    std::string id;
    std::string version;
    std::vector<std::string> sources;
    std::unordered_map<std::string, std::string> dependencies;
    std::string path;
};

struct ResolvedPackage {
    std::string id;
    std::string version;
    std::string rootPath;
    std::string source;
    std::string hash;
};

struct LoadedPackage {
    std::unique_ptr<Program> program;
    std::string rootPath;
    std::vector<std::string> sourceFiles;
    std::vector<std::string> modules;
    std::vector<PackageUse> packageUses;
    PackageManifest manifest;
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
