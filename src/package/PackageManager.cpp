#include "PackageManager.h"

#include "../diagnostics/Diagnostic.h"
#include "../lexer/Lexer.h"
#include "../parser/Parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
#include "PackageParsingInternal.h"

using namespace package_detail;

PackageManager::PackageManager(luna::macro::MacroProcessor macroProcessor)
    : mMacroProcessor(std::move(macroProcessor)) {}

bool PackageManager::load(const PackageRequest& request, LoadedPackage& result,
                          PackageGraph& graph,
                          std::vector<diagnostic::Diagnostic>& errors) const {
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
    PackageManifest manifest;
    const fs::path manifestPath = isDirectory ? input / "luna.package" : fs::path{};
    const bool hasManifest = isDirectory && fs::is_regular_file(manifestPath, ec);
    if (isDirectory) {
        graph.rootPath = fs::absolute(input, ec).string();
        if (hasManifest) {
            if (!parsePackageManifest(manifestPath, manifest, errors)) return false;
            graph.manifestPath = manifestPath.string();
            if (!collectManifestSources(input, manifest, files, errors)) return false;
        } else {
            for (const auto& entry : fs::directory_iterator(input, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec) && entry.path().extension() == ".luna")
                    files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
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
    result.manifest = manifest;
    result.program = std::make_unique<Program>();
    result.program->isPackage = isDirectory;
    if (hasManifest) result.program->packageName = manifest.id;
    for (const auto& [localName, capabilityId] : manifest.hostImports) {
        result.program->hostImports.push_back({});
        auto& hostImport = result.program->hostImports.back();
        hostImport.ownerPackageId = manifest.id;
        hostImport.localName = localName;
        hostImport.capabilityId = capabilityId;
        hostImport.sourcePath = manifest.path;
    }
    std::set<std::string> modules;
    std::unordered_map<std::string, PackageUse> usesByAlias;

    std::unordered_map<std::string, const std::string*> overlays;
    std::set<std::string> sourcePaths;
    for (const auto& file : files)
        sourcePaths.insert(fs::absolute(file, ec).lexically_normal().string());
    for (const auto& overlay : request.overlays) {
        const std::string path = fs::absolute(overlay.path, ec)
            .lexically_normal().string();
        if (!sourcePaths.count(path)) {
            errors.push_back(diagnostic::format(
                "package", "overlay path is not a source of the selected package: '" +
                    overlay.path + "'", overlay.path, 0, 0,
                "select the package containing the overlaid document"));
            return false;
        }
        if (!overlays.emplace(path, &overlay.source).second) {
            errors.push_back(diagnostic::format(
                "package", "duplicate source overlay: '" + overlay.path + "'",
                overlay.path, 0, 0,
                "provide at most one overlay for each source path"));
            return false;
        }
    }

    bool success = true;
    for (const auto& file : files) {
        std::unique_ptr<Program> sourceProgram;
        const auto overlay = overlays.find(
            fs::absolute(file, ec).lexically_normal().string());
        const std::string* overlaySource = overlay == overlays.end()
            ? nullptr : overlay->second;
        if (!parseSource(
                file, mMacroProcessor, sourceProgram, errors, overlaySource)) {
            success = false;
            continue;
        }
        graph.sourceUnits.push_back(file.string());
        result.sourceFiles.push_back(file.string());
        result.program->sourceFiles.push_back(file.string());

        modules.insert(sourceProgram->modulePath);
        for (const auto& sourceUse : sourceProgram->packageUses) {
            PackageUse use{sourceUse.packageId, sourceUse.alias,
                           sourceUse.sourcePath, sourceUse.line, sourceUse.col};
            auto existing = usesByAlias.find(use.alias);
            if (existing != usesByAlias.end() &&
                existing->second.packageId != use.packageId) {
                errors.push_back(diagnostic::format(
                    "package", "package alias '" + use.alias + "' refers to both '" +
                    existing->second.packageId + "' and '" + use.packageId + "'",
                    use.sourcePath, use.line, use.column,
                    "each package alias must identify exactly one Package ID"));
                success = false;
                continue;
            }
            usesByAlias.emplace(use.alias, std::move(use));
        }

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
            assignDeclarationOwner(declaration.get(), result.program->packageName.empty()
                ? sourceProgram->packageName : result.program->packageName);
        for (auto& declaration : sourceProgram->declarations)
            result.program->declarations.push_back(std::move(declaration));
    }

    if (result.program->isPackage && result.program->packageName.empty())
        result.program->packageName = input.filename().string();

    if (hasManifest) {
        const fs::path workspacePath = findWorkspace(input);
        std::unordered_map<std::string, ResolvedPackage> workspacePackages;
        if (!workspacePath.empty()) {
            graph.workspacePath = workspacePath.string();
            std::vector<std::string> members;
            if (!parseWorkspace(workspacePath, members, errors)) return false;
            const fs::path workspaceRoot = workspacePath.parent_path();
            for (const auto& member : members) {
                const fs::path memberPath = workspaceRoot / member;
                PackageManifest memberManifest;
                if (!parsePackageManifest(memberPath / "luna.package", memberManifest, errors))
                    return false;
                ResolvedPackage resolved{memberManifest.id, memberManifest.version,
                                         fs::absolute(memberPath, ec).string(),
                                         "workspace:" + member, {}};
                if (!workspacePackages.emplace(resolved.id, resolved).second) {
                    errors.push_back(diagnostic::format(
                        "package", "workspace contains duplicate Package ID '" + resolved.id + "'",
                        workspacePath.string(), 0, 0,
                        "every workspace member must have a unique Package ID"));
                    return false;
                }
            }
        }
        std::vector<ResolvedPackage> lockedPackages;
        if (!manifest.dependencies.empty()) {
            if (workspacePath.empty()) {
                errors.push_back(diagnostic::format(
                    "package", "package dependencies require a discoverable luna.workspace",
                    manifestPath.string(), 0, 0,
                    "place the package under a workspace; registry resolution is not enabled yet"));
                success = false;
            } else {
                const fs::path lockPath = workspacePath.parent_path() / "luna.lock";
                graph.lockPath = lockPath.string();
                if (!fs::is_regular_file(lockPath, ec)) {
                    errors.push_back(diagnostic::format(
                        "package", "workspace dependencies require luna.lock",
                        workspacePath.string(), 0, 0,
                        "generate and commit a deterministic workspace lock file"));
                    success = false;
                } else if (!parseLock(lockPath, lockedPackages, errors)) {
                    return false;
                }
            }
        }
        for (const auto& [alias, use] : usesByAlias) {
            (void)alias;
            auto constraint = manifest.dependencies.find(use.packageId);
            if (constraint == manifest.dependencies.end()) {
                errors.push_back(diagnostic::format(
                    "package", "using Package ID '" + use.packageId +
                    "' is not declared in [dependencies]",
                    use.sourcePath, use.line, use.column,
                    "add the Package ID and version constraint to luna.package"));
                success = false;
                continue;
            }
            auto resolved = workspacePackages.find(use.packageId);
            if (resolved == workspacePackages.end()) {
                errors.push_back(diagnostic::format(
                    "package", "cannot resolve Package ID '" + use.packageId + "' locally",
                    manifestPath.string(), 0, 0,
                    "add it as a luna.workspace member; registry resolution is not enabled yet"));
                success = false;
                continue;
            }
            if (constraint->second != resolved->second.version) {
                errors.push_back(diagnostic::format(
                    "package", "workspace package '" + use.packageId + "' has version '" +
                    resolved->second.version + "', expected '" + constraint->second + "'",
                    manifestPath.string(), 0, 0,
                    "use an exact matching version during the Alpha manifest stage"));
                success = false;
                continue;
            }
            const std::string usedPackageId = use.packageId;
            auto locked = std::find_if(
                lockedPackages.begin(), lockedPackages.end(),
                [&](const ResolvedPackage& item) { return item.id == usedPackageId; });
            if (locked == lockedPackages.end() ||
                locked->version != resolved->second.version ||
                locked->source != resolved->second.source) {
                errors.push_back(diagnostic::format(
                    "package", "luna.lock does not pin the resolved package '" +
                    use.packageId + "'",
                    graph.lockPath, 0, 0,
                    "regenerate the lock file after changing workspace members or versions"));
                success = false;
                continue;
            }
            auto pinned = resolved->second;
            pinned.hash = locked->hash;
            graph.resolvedPackages.push_back(std::move(pinned));
        }
        std::sort(graph.resolvedPackages.begin(), graph.resolvedPackages.end(),
                  [](const ResolvedPackage& left, const ResolvedPackage& right) {
                      return left.id < right.id;
                  });

        // Parse the complete local dependency closure into the same typed
        // compilation unit. Visibility is still enforced by semantic name
        // resolution: only exported declarations can cross a Package ID.
        std::set<std::string> loadedDependencies;
        std::set<std::string> loadingDependencies;
        std::function<bool(const std::string&)> loadDependency;
        loadDependency = [&](const std::string& packageId) -> bool {
            if (loadedDependencies.count(packageId)) return true;
            if (!loadingDependencies.insert(packageId).second) {
                errors.push_back(diagnostic::format(
                    "package", "cyclic package dependency involving '" + packageId + "'",
                    workspacePath.string(), 0, 0,
                    "package dependencies must form an acyclic graph"));
                return false;
            }
            auto resolved = workspacePackages.find(packageId);
            if (resolved == workspacePackages.end()) return false;
            PackageManifest dependencyManifest;
            const fs::path dependencyRoot(resolved->second.rootPath);
            if (!parsePackageManifest(dependencyRoot / "luna.package",
                                      dependencyManifest, errors))
                return false;
            for (const auto& [localName, capabilityId] :
                 dependencyManifest.hostImports) {
                result.program->hostImports.push_back({});
                auto& hostImport = result.program->hostImports.back();
                hostImport.ownerPackageId = dependencyManifest.id;
                hostImport.localName = localName;
                hostImport.capabilityId = capabilityId;
                hostImport.sourcePath = dependencyManifest.path;
            }
            std::vector<fs::path> dependencyFiles;
            if (!collectManifestSources(dependencyRoot, dependencyManifest,
                                        dependencyFiles, errors))
                return false;

            std::unordered_map<std::string, PackageUse> dependencyUses;
            for (const auto& dependencyFile : dependencyFiles) {
                std::unique_ptr<Program> sourceProgram;
                if (!parseSource(dependencyFile, mMacroProcessor, sourceProgram, errors))
                    return false;
                if (!sourceProgram->packageName.empty() &&
                    sourceProgram->packageName != packageId) {
                    errors.push_back(diagnostic::format(
                        "package", "package name '" + sourceProgram->packageName +
                        "' does not match manifest Package ID '" + packageId + "'",
                        dependencyFile.string(), 0, 0,
                        "dependency sources must declare their owning Package ID"));
                    return false;
                }
                result.program->sourceFiles.push_back(dependencyFile.string());
                for (const auto& sourceUse : sourceProgram->packageUses) {
                    PackageUse use{sourceUse.packageId, sourceUse.alias,
                                   sourceUse.sourcePath, sourceUse.line, sourceUse.col};
                    auto existing = dependencyUses.find(use.alias);
                    if (existing != dependencyUses.end() &&
                        existing->second.packageId != use.packageId) {
                        errors.push_back(diagnostic::format(
                            "package", "package alias '" + use.alias +
                            "' is ambiguous inside '" + packageId + "'",
                            use.sourcePath, use.line, use.column,
                            "aliases are shared by all modules of one package"));
                        return false;
                    }
                    dependencyUses.emplace(use.alias, use);
                }
                for (auto& declaration : sourceProgram->declarations) {
                    assignDeclarationOwner(declaration.get(), packageId);
                    result.program->declarations.push_back(std::move(declaration));
                }
            }

            for (const auto& [alias, use] : dependencyUses) {
                (void)alias;
                const std::string usedPackageId = use.packageId;
                auto constraint = dependencyManifest.dependencies.find(use.packageId);
                auto nested = workspacePackages.find(use.packageId);
                if (constraint == dependencyManifest.dependencies.end() ||
                    nested == workspacePackages.end() ||
                    constraint->second != nested->second.version) {
                    errors.push_back(diagnostic::format(
                        "package", "cannot resolve dependency '" + use.packageId +
                        "' used by '" + packageId + "'",
                        use.sourcePath, use.line, use.column,
                        "declare an exact-version local workspace dependency"));
                    return false;
                }
                auto locked = std::find_if(
                    lockedPackages.begin(), lockedPackages.end(),
                    [&](const ResolvedPackage& item) { return item.id == usedPackageId; });
                if (locked == lockedPackages.end() ||
                    locked->version != nested->second.version ||
                    locked->source != nested->second.source) {
                    errors.push_back(diagnostic::format(
                        "package", "luna.lock does not pin transitive package '" +
                        use.packageId + "'", graph.lockPath, 0, 0,
                        "regenerate the workspace lock file"));
                    return false;
                }
                result.program->packageUses.push_back({});
                auto& mergedUse = result.program->packageUses.back();
                mergedUse.ownerPackageId = packageId;
                mergedUse.packageId = use.packageId;
                mergedUse.alias = use.alias;
                mergedUse.sourcePath = use.sourcePath;
                mergedUse.line = use.line;
                mergedUse.col = use.column;
                if (std::none_of(graph.resolvedPackages.begin(),
                                 graph.resolvedPackages.end(),
                                 [&](const ResolvedPackage& item) {
                                     return item.id == usedPackageId;
                                 })) {
                    auto pinned = nested->second;
                    pinned.hash = locked->hash;
                    graph.resolvedPackages.push_back(std::move(pinned));
                }
                if (!loadDependency(use.packageId)) return false;
            }
            loadingDependencies.erase(packageId);
            loadedDependencies.insert(packageId);
            return true;
        };
        const auto directDependencies = graph.resolvedPackages;
        for (const auto& dependency : directDependencies)
            if (!loadDependency(dependency.id)) success = false;
        std::sort(graph.resolvedPackages.begin(), graph.resolvedPackages.end(),
                  [](const ResolvedPackage& left, const ResolvedPackage& right) {
                      return left.id < right.id;
                  });
        graph.resolvedPackages.erase(
            std::unique(graph.resolvedPackages.begin(), graph.resolvedPackages.end(),
                        [](const ResolvedPackage& left, const ResolvedPackage& right) {
                            return left.id == right.id;
                        }),
            graph.resolvedPackages.end());
    }

    for (const auto& [alias, use] : usesByAlias) {
        (void)alias;
        if (use.packageId == result.program->packageName) {
            errors.push_back(diagnostic::format(
                "package", "package cannot use itself as '" + use.alias + "'",
                use.sourcePath, use.line, use.column,
                "refer to modules in the current package directly"));
            success = false;
            continue;
        }
        result.packageUses.push_back(use);
        result.program->packageUses.push_back({});
        auto& mergedUse = result.program->packageUses.back();
        mergedUse.ownerPackageId = result.program->packageName;
        mergedUse.packageId = use.packageId;
        mergedUse.alias = use.alias;
        mergedUse.sourcePath = use.sourcePath;
        mergedUse.line = use.line;
        mergedUse.col = use.column;
        graph.dependencies.push_back(use.packageId);
        graph.dependencyUses.push_back(use);
    }
    std::sort(result.packageUses.begin(), result.packageUses.end(),
              [](const PackageUse& left, const PackageUse& right) {
                  return left.alias < right.alias;
              });
    std::sort(graph.dependencies.begin(), graph.dependencies.end());
    graph.dependencies.erase(
        std::unique(graph.dependencies.begin(), graph.dependencies.end()),
        graph.dependencies.end());
    result.modules.assign(modules.begin(), modules.end());
    result.program->sourceModules = result.modules;
    graph.modules = result.modules;

    return success && errors.empty();
}
