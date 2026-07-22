#include "SymbolTable.h"

SymbolTable::SymbolTable() {
    enterScope(); // global scope
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
    for (auto it = mScopes.rbegin(); it != mScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

SymbolInfo* SymbolTable::lookupLinkage(const std::string& name) {
    auto found = mLinkageSymbols.find(name);
    return found == mLinkageSymbols.end() ? nullptr : &found->second;
}

bool SymbolTable::hasInCurrentScope(const std::string& name) const {
    return mScopes.back().count(name) > 0;
}

void SymbolTable::defineType(const std::string& name, TypePtr type) {
    mTypeMap[name] = type;
}

TypePtr SymbolTable::lookupType(const std::string& name) const {
    auto it = mTypeMap.find(name);
    return it == mTypeMap.end() ? nullptr : it->second;
}

std::unordered_map<std::string, SymbolInfo> SymbolTable::visibleSymbols() const {
    std::unordered_map<std::string, SymbolInfo> result;
    for (const auto& scope : mScopes) {
        for (const auto& [name, info] : scope) result[name] = info;
    }
    return result;
}
