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

namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string withoutComment(const std::string& line) {
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (escaped) { escaped = false; continue; }
        if (character == '\\' && quoted) { escaped = true; continue; }
        if (character == '"') quoted = !quoted;
        else if (character == '#' && !quoted) return line.substr(0, index);
    }
    return line;
}

bool parseTomlString(const std::string& text, std::string& value) {
    const std::string input = trim(text);
    if (input.size() < 2 || input.front() != '"' || input.back() != '"') return false;
    value.clear();
    bool escaped = false;
    for (size_t index = 1; index + 1 < input.size(); ++index) {
        const char character = input[index];
        if (escaped) {
            if (character == 'n') value.push_back('\n');
            else if (character == 't') value.push_back('\t');
            else value.push_back(character);
            escaped = false;
        } else if (character == '\\') escaped = true;
        else if (character == '"') return false;
        else value.push_back(character);
    }
    return !escaped;
}

bool parseTomlStringArray(const std::string& text, std::vector<std::string>& values) {
    const std::string input = trim(text);
    if (input.size() < 2 || input.front() != '[' || input.back() != ']') return false;
    values.clear();
    std::string body = trim(input.substr(1, input.size() - 2));
    if (body.empty()) return true;
    size_t begin = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index <= body.size(); ++index) {
        const char character = index < body.size() ? body[index] : ',';
        if (escaped) { escaped = false; continue; }
        if (character == '\\' && quoted) { escaped = true; continue; }
        if (character == '"') quoted = !quoted;
        if (character == ',' && !quoted) {
            std::string item;
            if (!parseTomlString(body.substr(begin, index - begin), item)) return false;
            values.push_back(std::move(item));
            begin = index + 1;
        }
    }
    return !quoted;
}

bool splitTomlAssignment(const std::string& line, std::string& key, std::string& value) {
    bool quoted = false;
    for (size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') quoted = !quoted;
        if (line[index] == '=' && !quoted) {
            key = trim(line.substr(0, index));
            value = trim(line.substr(index + 1));
            if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
                std::string decoded;
                if (!parseTomlString(key, decoded)) return false;
                key = std::move(decoded);
            }
            return !key.empty() && !value.empty();
        }
    }
    return false;
}

void manifestError(std::vector<diagnostic::Diagnostic>& errors,
                   const fs::path& path,
                   int line, const std::string& message, const std::string& hint) {
    errors.push_back(diagnostic::format(
        "package", message, path.string(), line, 1, hint));
}

bool parsePackageManifest(const fs::path& path, PackageManifest& manifest,
                          std::vector<diagnostic::Diagnostic>& errors) {
    std::ifstream file(path);
    if (!file) return false;
    manifest = {};
    manifest.path = path.string();
    std::string section;
    std::string raw;
    int lineNumber = 0;
    bool ok = true;
    while (std::getline(file, raw)) {
        ++lineNumber;
        const std::string line = trim(withoutComment(raw));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (section != "package" && section != "dependencies") {
                manifestError(errors, path, lineNumber, "unknown manifest section '[" + section + "]'",
                              "supported sections are [package] and [dependencies]");
                ok = false;
            }
            continue;
        }
        std::string key, value;
        if (!splitTomlAssignment(line, key, value)) {
            manifestError(errors, path, lineNumber, "invalid TOML assignment", "write `key = value`");
            ok = false; continue;
        }
        if (section == "package") {
            if (key == "id") ok = parseTomlString(value, manifest.id) && ok;
            else if (key == "version") ok = parseTomlString(value, manifest.version) && ok;
            else if (key == "sources") ok = parseTomlStringArray(value, manifest.sources) && ok;
            else {
                manifestError(errors, path, lineNumber, "unknown [package] key '" + key + "'",
                              "supported keys are id, version, and sources");
                ok = false;
            }
        } else if (section == "dependencies") {
            std::string constraint;
            if (!parseTomlString(value, constraint)) ok = false;
            else manifest.dependencies[key] = std::move(constraint);
        } else {
            manifestError(errors, path, lineNumber, "manifest key appears outside a section",
                          "start with [package]");
            ok = false;
        }
        if (!ok && errors.empty())
            manifestError(errors, path, lineNumber, "invalid TOML value", "strings must be quoted");
    }
    if (manifest.id.empty() || manifest.version.empty() || manifest.sources.empty()) {
        manifestError(errors, path, 0, "incomplete package manifest",
                      "[package] requires id, version, and a non-empty sources array");
        ok = false;
    }
    return ok;
}

bool parseWorkspace(const fs::path& path, std::vector<std::string>& members,
                    std::vector<diagnostic::Diagnostic>& errors) {
    std::ifstream file(path);
    if (!file) return false;
    std::string section, raw;
    int lineNumber = 0;
    bool foundMembers = false;
    while (std::getline(file, raw)) {
        ++lineNumber;
        const std::string line = trim(withoutComment(raw));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (section != "workspace") {
                manifestError(errors, path, lineNumber, "unknown workspace section '[" + section + "]'",
                              "only [workspace] is supported");
                return false;
            }
            continue;
        }
        std::string key, value;
        if (section != "workspace" || !splitTomlAssignment(line, key, value) ||
            key != "members" || !parseTomlStringArray(value, members)) {
            manifestError(errors, path, lineNumber, "invalid workspace declaration",
                          "write `[workspace]` followed by `members = [\"path\"]`");
            return false;
        }
        foundMembers = true;
    }
    if (!foundMembers || members.empty()) {
        manifestError(errors, path, 0, "workspace has no members", "add at least one member path");
        return false;
    }
    return true;
}

bool parseLock(const fs::path& path, std::vector<ResolvedPackage>& packages,
               std::vector<diagnostic::Diagnostic>& errors) {
    std::ifstream file(path);
    if (!file) return false;
    std::string raw;
    int lineNumber = 0;
    ResolvedPackage current;
    bool inPackage = false;
    auto finish = [&]() -> bool {
        if (!inPackage) return true;
        if (current.id.empty() || current.version.empty() || current.source.empty() ||
            current.hash.empty()) {
            manifestError(errors, path, lineNumber, "incomplete [[package]] lock entry",
                          "lock entries require id, version, source, and hash");
            return false;
        }
        packages.push_back(std::move(current));
        current = {};
        return true;
    };
    while (std::getline(file, raw)) {
        ++lineNumber;
        const std::string line = trim(withoutComment(raw));
        if (line.empty()) continue;
        if (line == "[[package]]") {
            if (!finish()) return false;
            inPackage = true;
            continue;
        }
        if (!inPackage) {
            manifestError(errors, path, lineNumber, "lock key appears outside [[package]]",
                          "start each entry with [[package]]");
            return false;
        }
        std::string key, encoded, value;
        if (!splitTomlAssignment(line, key, encoded) || !parseTomlString(encoded, value)) {
            manifestError(errors, path, lineNumber, "invalid lock assignment",
                          "lock values must be quoted strings");
            return false;
        }
        if (key == "id") current.id = value;
        else if (key == "version") current.version = value;
        else if (key == "source") current.source = value;
        else if (key == "hash") current.hash = value;
        else {
            manifestError(errors, path, lineNumber, "unknown lock key '" + key + "'",
                          "supported keys are id, version, source, and hash");
            return false;
        }
    }
    return finish();
}

fs::path findWorkspace(const fs::path& start) {
    std::error_code ec;
    fs::path current = fs::absolute(start, ec);
    while (!current.empty()) {
        const fs::path candidate = current / "luna.workspace";
        if (fs::is_regular_file(candidate, ec)) return candidate;
        const fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

bool readSource(const fs::path& path, std::string& source,
                std::vector<diagnostic::Diagnostic>& errors) {
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
                 std::vector<diagnostic::Diagnostic>& errors) {
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

bool collectManifestSources(const fs::path& packageRoot,
                            const PackageManifest& manifest,
                            std::vector<fs::path>& files,
                            std::vector<diagnostic::Diagnostic>& errors) {
    std::error_code ec;
    for (const auto& sourceRoot : manifest.sources) {
        const fs::path relative(sourceRoot);
        if (relative.is_absolute() ||
            std::find(relative.begin(), relative.end(), fs::path("..")) != relative.end()) {
            errors.push_back(diagnostic::format(
                "package", "source root escapes the package: '" + sourceRoot + "'",
                manifest.path, 0, 0,
                "source roots must be relative paths inside the package"));
            return false;
        }
        const fs::path root = packageRoot / relative;
        if (!fs::exists(root, ec)) {
            errors.push_back(diagnostic::format(
                "package", "manifest source root does not exist: '" + sourceRoot + "'",
                manifest.path, 0, 0, "create the path or update sources"));
            return false;
        }
        if (fs::is_regular_file(root, ec) && root.extension() == ".luna")
            files.push_back(root);
        else if (fs::is_directory(root, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec) && entry.path().extension() == ".luna")
                    files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return true;
}

void assignDeclarationOwner(Decl* declaration, const std::string& packageId) {
    if (!declaration) return;
    declaration->packageId = packageId;
    if (auto* implementation = dynamic_cast<ImplDecl*>(declaration)) {
        for (auto& method : implementation->methods) {
            method->packageId = packageId;
            method->modulePath = declaration->modulePath;
        }
    }
}

} // namespace

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
    std::set<std::string> modules;
    std::unordered_map<std::string, PackageUse> usesByAlias;

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
