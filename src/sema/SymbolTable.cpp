#include "SymbolTable.h"
#include "PredefinedTypes.h"

SymbolTable::SymbolTable() {
    enterScope(); // global scope
    for (const auto& definition : predefinedTypes()) {
        if (definition.form == PredefinedTypeForm::Atomic)
            mTypeMap.emplace(std::string(definition.name), definition.atomicType);
    }
}

void SymbolTable::enterScope() {
    mScopes.emplace_back();
}

void SymbolTable::exitScope() {
    if (mScopes.size() > 1) mScopes.pop_back();
}

bool SymbolTable::define(const std::string& name, SymbolInfo info) {
    auto& current = mScopes.back();
    if (current.count(name)) return false;
    current[name] = std::move(info);
    return true;
}

bool SymbolTable::defineAtRoot(const std::string& name, SymbolInfo info) {
    auto& root = mScopes.front();
    if (root.count(name)) return false;
    root[name] = std::move(info);
    return true;
}

void SymbolTable::defineLinkage(const std::string& name, SymbolInfo info) {
    mLinkageSymbols[name] = std::move(info);
}

SymbolInfo* SymbolTable::lookup(const std::string& name) {
    const size_t barrier = mVisibilityBarriers.empty()
        ? size_t{1} : mVisibilityBarriers.back();
    for (size_t depth = mScopes.size(); depth > barrier; --depth) {
        auto found = mScopes[depth - 1].find(name);
        if (found != mScopes[depth - 1].end()) return &found->second;
    }
    auto root = mScopes.front().find(name);
    if (root != mScopes.front().end()) return &root->second;
    return nullptr;
}

size_t SymbolTable::lookupDepth(const std::string& name) const {
    const size_t barrier = mVisibilityBarriers.empty()
        ? size_t{1} : mVisibilityBarriers.back();
    for (size_t depth = mScopes.size(); depth > barrier; --depth) {
        if (mScopes[depth - 1].count(name)) return depth - 1;
    }
    if (mScopes.front().count(name)) return 0;
    return static_cast<size_t>(-1);
}

SymbolInfo* SymbolTable::lookupLinkage(const std::string& name) {
    auto found = mLinkageSymbols.find(name);
    return found == mLinkageSymbols.end() ? nullptr : &found->second;
}

bool SymbolTable::hasInCurrentScope(const std::string& name) const {
    return mScopes.back().count(name) > 0;
}

bool SymbolTable::defineType(const std::string& name, TypePtr type) {
    if (isPredefinedTypeName(name)) return false;
    mTypeMap[name] = type;
    return true;
}

TypePtr SymbolTable::lookupType(const std::string& name) const {
    auto it = mTypeMap.find(name);
    return it == mTypeMap.end() ? nullptr : it->second;
}

bool SymbolTable::isPredefinedType(const std::string& name) const {
    return isPredefinedTypeName(name);
}

std::unordered_map<std::string, SymbolInfo> SymbolTable::visibleSymbols() const {
    std::unordered_map<std::string, SymbolInfo> result = mScopes.front();
    const size_t barrier = mVisibilityBarriers.empty()
        ? size_t{1} : mVisibilityBarriers.back();
    for (size_t depth = barrier; depth < mScopes.size(); ++depth)
        for (const auto& [name, info] : mScopes[depth]) result[name] = info;
    return result;
}

void SymbolTable::enterIsolatedScope() {
    mVisibilityBarriers.push_back(mScopes.size());
    enterScope();
}

void SymbolTable::exitIsolatedScope() {
    if (mVisibilityBarriers.empty()) return;
    const size_t barrier = mVisibilityBarriers.back();
    while (mScopes.size() > barrier) exitScope();
    mVisibilityBarriers.pop_back();
}
