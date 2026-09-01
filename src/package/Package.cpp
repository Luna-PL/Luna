#include "Package.h"
#include "PackageManager.h"

namespace {

std::string publicDeclarationName(const Decl* declaration,
                                  const std::string& sourceName) {
    if (!declaration || declaration->generatedSymbolName.empty() ||
        declaration->generatedSymbolName == sourceName)
        return sourceName;
    return declaration->generatedSymbolName;
}

} // namespace

const char* packageKindName(PackageKind kind) {
    switch (kind) {
        case PackageKind::Application: return "application";
        case PackageKind::Library: return "library";
        case PackageKind::Unspecified: return "unspecified";
    }
    return "unspecified";
}

bool PackageLoader::load(const std::string& path, LoadedPackage& result,
                         std::vector<diagnostic::Diagnostic>& errors) {
    PackageManager manager;
    PackageGraph graph;
    PackageRequest request;
    request.inputPath = path;
    return manager.load(request, result, graph, errors);
}

std::vector<PackageExport> PackageLoader::exports(const Program* program) {
    std::vector<PackageExport> result;
    if (!program) return result;

    for (const auto& declaration : program->declarations) {
        if (!declaration->isExported) continue;
        if (!declaration->packageId.empty() &&
            declaration->packageId != program->packageName)
            continue;
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(function, function->name), "function", function->modulePath});
        else if (auto* fragment = dynamic_cast<FragmentDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(fragment, fragment->name), "fragment", fragment->modulePath});
        else if (auto* slot = dynamic_cast<SlotDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(slot, slot->name), "slot", slot->modulePath});
        else if (auto* structure = dynamic_cast<StructDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(structure, structure->name), "struct", structure->modulePath});
        else if (auto* enumeration = dynamic_cast<EnumDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(enumeration, enumeration->name), "enum", enumeration->modulePath});
        else if (auto* trait = dynamic_cast<TraitDecl*>(declaration.get()))
            result.push_back({publicDeclarationName(trait, trait->name), "trait", trait->modulePath});
    }
    return result;
}
