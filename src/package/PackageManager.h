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
    // Dependency and lock nodes are deliberately empty in the local-source
    // implementation, but they belong to this component rather than Driver.
    std::vector<std::string> dependencies;
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
