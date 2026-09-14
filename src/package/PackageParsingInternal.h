#pragma once

#include "PackageManager.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace package_detail {

bool parsePackageManifest(
    const std::filesystem::path& path, PackageManifest& manifest,
    std::vector<diagnostic::Diagnostic>& errors);
bool parseWorkspace(
    const std::filesystem::path& path, std::vector<std::string>& members,
    std::vector<diagnostic::Diagnostic>& errors);
bool parseLock(
    const std::filesystem::path& path,
    std::vector<ResolvedPackage>& packages,
    std::vector<diagnostic::Diagnostic>& errors);
bool parseSource(
    const std::filesystem::path& path,
    const luna::macro::MacroProcessor& macroProcessor,
    std::unique_ptr<Program>& program,
    std::vector<diagnostic::Diagnostic>& errors,
    const std::string* overlaySource = nullptr);
bool collectManifestSources(
    const std::filesystem::path& packageRoot,
    const PackageManifest& manifest,
    std::vector<std::filesystem::path>& files,
    std::vector<diagnostic::Diagnostic>& errors);
std::filesystem::path findWorkspace(
    const std::filesystem::path& start);
void assignDeclarationOwner(
    Decl* declaration, const std::string& packageId);

} // namespace package_detail
