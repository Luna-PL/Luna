#pragma once

#include "../parser/AST.h"
#include "diagnostics/Diagnostic.h"
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

enum class PackageKind {
    Unspecified,
    Application,
    Library,
};

struct PackageManifest {
    std::string id;
    std::string version;
    PackageKind kind = PackageKind::Unspecified;
    std::vector<std::string> sources;
    std::unordered_map<std::string, std::string> dependencies;
    // package-local module-qualified extern declaration -> stable host
    // capability ID. ContractId remains compiler-derived.
    std::unordered_map<std::string, std::string> hostImports;
    std::string path;
};

const char* packageKindName(PackageKind kind);

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
                     std::vector<diagnostic::Diagnostic>& errors);

    // Returns the package's explicit public surface. Declarations not marked
    // with `export` are intentionally absent from this interface.
    static std::vector<PackageExport> exports(const Program* program);
};
