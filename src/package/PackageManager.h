#pragma once

#include "Package.h"
#include "../macro/MacroProcessor.h"

#include <string>
#include <vector>

struct PackageRequest {
    std::string inputPath;
};

struct PackageGraph {
    std::string rootPath;
    std::vector<std::string> sourceUnits;
    // Dependency edges and locked local workspace nodes belong to the package
    // layer; the driver consumes only the assembled typed source graph.
    std::vector<std::string> dependencies;
    std::vector<PackageUse> dependencyUses;
    std::vector<std::string> modules;
    std::vector<ResolvedPackage> resolvedPackages;
    std::string manifestPath;
    std::string workspacePath;
    std::string lockPath;
    std::vector<std::string> enabledCapabilities;
};

class PackageManager {
public:
    explicit PackageManager(
        luna::macro::MacroProcessor macroProcessor = luna::macro::MacroProcessor());

    bool load(const PackageRequest& request, LoadedPackage& result,
              PackageGraph& graph, std::vector<std::string>& errors) const;

private:
    luna::macro::MacroProcessor mMacroProcessor;
};
