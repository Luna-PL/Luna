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

namespace package_detail {

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
            if (section != "package" && section != "dependencies" &&
                section != "host-imports") {
                manifestError(errors, path, lineNumber, "unknown manifest section '[" + section + "]'",
                              "supported sections are [package], [dependencies], and [host-imports]");
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
            else if (key == "kind") {
                std::string kind;
                if (!parseTomlString(value, kind)) {
                    ok = false;
                } else if (kind == "application") {
                    manifest.kind = PackageKind::Application;
                } else if (kind == "library") {
                    manifest.kind = PackageKind::Library;
                } else {
                    manifestError(
                        errors, path, lineNumber,
                        "unknown package kind '" + kind + "'",
                        "use kind = \"application\" or kind = \"library\"");
                    ok = false;
                }
            }
            else if (key == "sources") ok = parseTomlStringArray(value, manifest.sources) && ok;
            else {
                manifestError(errors, path, lineNumber, "unknown [package] key '" + key + "'",
                              "supported keys are id, version, kind, and sources");
                ok = false;
            }
        } else if (section == "dependencies") {
            std::string constraint;
            if (!parseTomlString(value, constraint)) ok = false;
            else manifest.dependencies[key] = std::move(constraint);
        } else if (section == "host-imports") {
            std::string capability;
            if (!parseTomlString(value, capability)) {
                ok = false;
            } else if (key.empty() || capability.empty()) {
                manifestError(errors, path, lineNumber,
                              "host import name and capability must be non-empty",
                              "map a module-qualified extern declaration to a stable capability ID");
                ok = false;
            } else if (!manifest.hostImports.emplace(key, std::move(capability)).second) {
                manifestError(errors, path, lineNumber,
                              "duplicate host import '" + key + "'",
                              "declare each host import exactly once");
                ok = false;
            }
        } else {
            manifestError(errors, path, lineNumber, "manifest key appears outside a section",
                          "start with [package]");
            ok = false;
        }
        if (!ok && errors.empty())
            manifestError(errors, path, lineNumber, "invalid TOML value", "strings must be quoted");
    }
    if (manifest.id.empty() || manifest.version.empty() ||
        manifest.kind == PackageKind::Unspecified || manifest.sources.empty()) {
        manifestError(errors, path, 0, "incomplete package manifest",
                      "[package] requires id, version, kind, and a non-empty sources array");
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
                 std::vector<diagnostic::Diagnostic>& errors,
                 const std::string* overlaySource) {
    std::string source;
    if (overlaySource)
        source = *overlaySource;
    else if (!readSource(path, source, errors))
        return false;

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

bool isPathInside(const fs::path& root, const fs::path& candidate,
                  std::error_code& ec) {
    ec.clear();
    const fs::path canonicalRoot = fs::canonical(root, ec);
    if (ec) return false;
    const fs::path canonicalCandidate = fs::canonical(candidate, ec);
    if (ec) return false;
    const fs::path relative = canonicalCandidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return false;
    return std::none_of(relative.begin(), relative.end(), [](const fs::path& part) {
        return part == fs::path("..");
    });
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
        if (!isPathInside(packageRoot, root, ec)) {
            errors.push_back(diagnostic::format(
                "package", "source root escapes the package through a filesystem link: '" +
                    sourceRoot + "'",
                manifest.path, 0, 0,
                "source roots and their resolved targets must remain inside the package"));
            return false;
        }
        if (fs::is_regular_file(root, ec) && root.extension() == ".luna")
            files.push_back(root);
        else if (fs::is_directory(root, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec) && entry.path().extension() == ".luna") {
                    if (!isPathInside(packageRoot, entry.path(), ec)) {
                        errors.push_back(diagnostic::format(
                            "package", "source file escapes the package through a filesystem link: '" +
                                entry.path().string() + "'",
                            manifest.path, 0, 0,
                            "source files and their resolved targets must remain inside the package"));
                        return false;
                    }
                    files.push_back(entry.path());
                }
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


} // namespace package_detail
