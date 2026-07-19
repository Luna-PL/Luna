#pragma once

#include "TypeSystem.h"
#include "../parser/AST.h"
#include <string>
#include <vector>
#include <unordered_map>

class TraitChecker {
public:
    TraitChecker();

    bool check(Program* program);
    const std::vector<std::string>& errors() const { return mErrors; }

    // Query whether a type satisfies a trait
    bool satisfies(const TypePtr& type, const std::string& traitName) const;

private:
    void registerTraits(Program* program);
    void registerImpls(Program* program);
    void checkConcreteFunction(FunctionDecl* decl);

    // Trait → methods ({name, paramTypes, returnType})
    struct MethodSig {
        std::string name;
        TypeVec paramTypes;
        TypePtr returnType;
    };
    std::unordered_map<std::string, std::vector<MethodSig>> mTraitSigs;

    // Trait → (typeName → [methodName → FunctionDecl*])
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::unordered_map<std::string, FunctionDecl*>>> mImplMap;

    void error(const std::string& msg);

    std::vector<std::string> mErrors;
    std::string mDiagnosticFile;
    int mDiagnosticLine = 0;
    int mDiagnosticCol = 0;
};
