#pragma once

#include "TypeSystem.h"
#include "SymbolTable.h"
#include "../parser/AST.h"
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <optional>
#include <variant>

struct TypeAST;
struct Program;
struct Expr;
struct Stmt;
struct FunctionDecl;
struct StructDecl;
struct EnumDecl;
struct TraitDecl;
struct ImplDecl;
struct ASTNode;

class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    bool analyze(Program* program);
    const std::vector<std::string>& errors() const { return mErrors; }
    SymbolTable& symTable() { return mSymTable; }

private:
    TypePtr resolveTypeAST(const TypeAST* ast,
                           const std::unordered_map<std::string, TypePtr>& bindings);

    void declareFunction(FunctionDecl* decl);
    void declareStruct(StructDecl* decl);
    void declareEnum(EnumDecl* decl);
    void declareTrait(TraitDecl* decl);
    void declareImpl(ImplDecl* decl);
    void declareFragment(FragmentDecl* decl);
    void validateFFIFunction(FunctionDecl* decl);
    bool isFFIType(const TypePtr& type, const std::string& context);

    void analyzeFunction(FunctionDecl* decl);
    void analyzeStruct(StructDecl* decl);
    void analyzeEnum(EnumDecl* decl);
    void analyzeTrait(TraitDecl* decl);
    void analyzeImpl(ImplDecl* decl);
    void analyzeSlotDecl(SlotDeclStmt* stmt);
    void analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn);
    void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn);
    void analyzeFragmentForSlot(FragmentDecl* fragment, const std::string& slotName,
                                const TypeVec& parameterTypes,
                                const std::unordered_map<std::string, SymbolInfo>& captures);

    TypePtr analyzeStmt(Stmt* stmt, TypePtr expectedReturnType);
    TypePtr analyzeBlock(BlockStmt* block, TypePtr expectedReturnType);
    TypePtr analyzeExpr(Expr* expr);
    TypePtr analyzeCall(CallExpr* call);
    TypePtr analyzeLaunch(LaunchExpr* launch);
    bool statementAlwaysReturns(const Stmt* stmt) const;
    bool blockAlwaysReturns(const BlockStmt* block) const;

    TypePtr declaredType(const TypeAST* ast,
                        const std::unordered_map<std::string, TypePtr>& bindings);
    TypePtr instantiateNominal(const TypePtr& type, const std::vector<TypePtr>& args);
    TypePtr resolved(const TypePtr& type);
    bool constrain(const TypePtr& actual, const TypePtr& expected,
                   const std::string& context);
    void requireBool(const TypePtr& type, const std::string& context);
    void requireNumeric(const TypePtr& type, const std::string& context);
    void requireInteger(const TypePtr& type, const std::string& context);
    void materializeInferredTypes(Program* program);
    std::unique_ptr<TypeAST> typeToAST(const TypePtr& type);
    void checkUnresolved(const TypePtr& type, const std::string& context);
    TypePtr analyzeReflectionCall(CallExpr* call, const std::string& name);
    std::optional<std::variant<int64_t, double, bool, std::string>>
    evaluateConstExpr(Expr* expr,
                      const std::unordered_map<std::string,
                      std::variant<int64_t, double, bool, std::string>>& locals = {});
    std::optional<std::variant<int64_t, double, bool, std::string>>
    evaluateConstFunction(FunctionDecl* function,
                          const std::vector<std::variant<int64_t, double, bool, std::string>>& args);
    bool evaluateConstBlock(BlockStmt* block,
                            std::unordered_map<std::string,
                            std::variant<int64_t, double, bool, std::string>>& locals,
                            std::optional<std::variant<int64_t, double, bool, std::string>>& result);
    void enterConstScope();
    void exitConstScope();
    void defineConst(const std::string& name,
                     const std::variant<int64_t, double, bool, std::string>& value);
    const std::variant<int64_t, double, bool, std::string>*
    lookupConst(const std::string& name) const;
    void enterSlotScope();
    void exitSlotScope();
    FunctionDecl* selectVersionedFunction(const std::string& name,
                                          const VersionSelector& selector,
                                          const ASTNode* useSite);
    FragmentDecl* selectFragment(const std::string& name,
                                 const std::optional<VersionSelector>& selector,
                                 const ASTNode* useSite);
    TypePtr selectVersionedNominal(const std::string& name,
                                   const VersionSelector& selector,
                                   const ASTNode* useSite);
    TraitDecl* selectVersionedTrait(const std::string& name,
                                    const VersionSelector& selector,
                                    const ASTNode* useSite);
    std::string resolveTraitRef(TraitRef& trait, const ASTNode* useSite);
    std::string typeIdentity(const TypePtr& type) const;
    std::string traitIdentity(const TraitDecl* trait) const;
    bool satisfiesTrait(const std::string& traitId, const TypePtr& type) const;

    FunctionDecl* findMatchingImpl(const std::string& traitName, const std::string& typeName,
                                   const std::string& methodName);

    // Template monomorphization: clone AST with type substitutions
    FunctionDecl* monomorphize(FunctionDecl* generic, const TypeVec& concreteTypes);

    void error(const std::string& msg, int line = 0, int col = 0);
    void setDiagnosticLocation(const ASTNode* node);

    SymbolTable mSymTable;
    // Exact TraitId → (exact target TypeId → [methodName → FunctionDecl*]).
    // Both keys are resolved during semantic analysis; raw source names never
    // participate in versioned trait coherence.
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::unordered_map<std::string, FunctionDecl*>>> mImpls;
    // Trait → typeParams
    std::unordered_map<std::string, std::vector<std::string>> mTraitTypeParams;
    // Trait → methods
    std::unordered_map<std::string, std::vector<std::pair<std::string, FunctionDecl*>>> mTraitMethods;
    struct VersionedTrait {
        SemanticVersion version;
        TraitDecl* decl;
    };
    std::unordered_map<std::string, TraitDecl*> mTraits;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<VersionedTrait>>> mVersionedTraits;
    std::unordered_map<std::string, TypePtr> mNominalTypes;
    struct VersionedNominal {
        SemanticVersion version;
        TypePtr type;
    };
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<FunctionDecl*>>> mVersionedFunctions;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<FragmentDecl*>>> mVersionedFragments;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<VersionedNominal>>> mVersionedNominals;

    std::vector<std::unique_ptr<FunctionDecl>> mGeneratedInstances;
    std::vector<std::string> mErrors;
    Program* mProgram = nullptr;
    std::string mDiagnosticFile;
    int mDiagnosticLine = 0;
    int mDiagnosticCol = 0;

    // For current function context
    TypePtr mCurrentReturnType;
    bool mInFunction = false;
    bool mInKernel = false;
    bool mCurrentFunctionReturnsLinear = false;
    bool mSawReturn = false;
    ConstraintSolver mConstraints;
    std::vector<std::pair<TypePtr, std::string>> mInferenceRoots;
    using ConstValue = std::variant<int64_t, double, bool, std::string>;
    std::vector<std::unordered_map<std::string, ConstValue>> mConstScopes;
    std::unordered_map<std::string, FunctionDecl*> mConstexprFunctions;
    int mConstEvaluationDepth = 0;
    struct SlotInfo {
        std::string name;
        TypeVec paramTypes;
        std::vector<std::string> paramNames;
        std::string defaultFragment;
        std::optional<VersionSelector> defaultFragmentSelector;
        std::string resolvedDefaultFragmentName;
        FragmentKind acceptedKind = FragmentKind::Interceptor;
        FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
        bool isImplicitCapture = false;
        bool isDynamic = false;
        TypePtr structuralType;
    };
    std::vector<std::unordered_map<std::string, SlotInfo>> mSlotScopes;
    std::vector<std::unordered_map<std::string, FragmentDecl*>> mApplyScopes;
    std::vector<std::unordered_map<std::string, std::vector<FragmentDecl*>>> mDynamicApplyScopes;
    std::unordered_map<std::string, FragmentDecl*> mFragments;
    const SlotInfo* mCurrentFragmentSlot = nullptr;
    FragmentDecl* mCurrentFragmentDecl = nullptr;
};
