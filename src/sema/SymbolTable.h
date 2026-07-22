#pragma once

#include "TypeSystem.h"
#include "../parser/AST.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

enum class SymbolKind {
    Variable, Function, Fragment, Slot, Struct, Trait, TypeParam, Metadata
};

struct FunctionDecl; // forward

struct SymbolInfo {
    SymbolKind kind;
    TypePtr type;                          // for Variable, TypeParam
    TypeVec paramTypes;                   // for Function params
    TypePtr returnType;                   // for Function return
    std::vector<std::string> typeParams;  // for generic Function/Struct
    bool isHeapAllocated = false;         // for Variable
    bool isLinear = false;                // for Variable/parameter
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
    bool isConst = false;                 // for compile-time immutable bindings
    bool isExported = false;              // for package-level declarations
    bool isExtern = false;                 // for C ABI declarations
    bool returnsLinear = false;            // owning function result
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    std::vector<luna::ownership::Contract> paramContracts;
    FunctionDecl* genericDecl = nullptr;  // for template instantiation
};

class SymbolTable {
public:
    SymbolTable();

    void enterScope();
    void exitScope();

    bool define(const std::string& name, SymbolInfo info);
    bool defineAtRoot(const std::string& name, SymbolInfo info);
    SymbolInfo* lookup(const std::string& name);
    bool hasInCurrentScope(const std::string& name) const;

    // Types: store the resolved TypePtr for user-defined types/traits
    void defineType(const std::string& name, TypePtr type);
    TypePtr lookupType(const std::string& name) const;
    std::unordered_map<std::string, SymbolInfo> visibleSymbols() const;

private:
    std::vector<std::unordered_map<std::string, SymbolInfo>> mScopes;
    std::unordered_map<std::string, TypePtr> mTypeMap; // global type/trait registry
};
