#pragma once

#include "SemanticContext.h"

// Component-scoped capabilities expose references to the one authoritative
// SemanticContext state. They deliberately preserve the existing algorithms'
// field names while making any new cross-component dependency a header change.

class BodyContextAccess final {
    friend class SemanticContext;

    explicit BodyContextAccess(SemanticContext& context);
    SemanticContext& mOwner;

public:
    using ConstValue = SemanticConstValue;
    using FromConversion = SemanticContext::FromConversion;
    using FromIteratorImplementation =
        SemanticContext::FromIteratorImplementation;

    decltype(SemanticContext::mConcepts)& mConcepts;
    decltype(SemanticContext::mConstexprFunctions)& mConstexprFunctions;
    decltype(SemanticContext::mConstraints)& mConstraints;
    decltype(SemanticContext::mCurrentFragmentDecl)& mCurrentFragmentDecl;
    decltype(SemanticContext::mCurrentFunctionReturnUsage)&
        mCurrentFunctionReturnUsage;
    decltype(SemanticContext::mCurrentFunctionReturnsLinear)&
        mCurrentFunctionReturnsLinear;
    decltype(SemanticContext::mCurrentModulePath)& mCurrentModulePath;
    decltype(SemanticContext::mCurrentPackageId)& mCurrentPackageId;
    decltype(SemanticContext::mCurrentReturnType)& mCurrentReturnType;
    decltype(SemanticContext::mDeclaredTypes)& mDeclaredTypes;
    decltype(SemanticContext::mFromConversions)& mFromConversions;
    decltype(SemanticContext::mFromIteratorImplementations)&
        mFromIteratorImplementations;
    decltype(SemanticContext::mFunctionFamilies)& mFunctionFamilies;
    decltype(SemanticContext::mGeneratedInstances)& mGeneratedInstances;
    decltype(SemanticContext::mImpls)& mImpls;
    decltype(SemanticContext::mInFunction)& mInFunction;
    decltype(SemanticContext::mInKernel)& mInKernel;
    decltype(SemanticContext::mInferenceRoots)& mInferenceRoots;
    decltype(SemanticContext::mIteratorStateCounter)& mIteratorStateCounter;
    decltype(SemanticContext::mMetadataSchemas)& mMetadataSchemas;
    decltype(SemanticContext::mProgram)& mProgram;
    decltype(SemanticContext::mSawReturn)& mSawReturn;
    decltype(SemanticContext::mSymbolCatalog)& mSymbolCatalog;
    decltype(SemanticContext::mSymTable)& mSymTable;
    decltype(SemanticContext::mTraitMethods)& mTraitMethods;
    decltype(SemanticContext::mTraits)& mTraits;

    void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn);
    TypePtr analyzeDeclarationReflectionCall(
        CallExpr* call, const std::string& name);
    TypePtr analyzeReflectionCall(
        CallExpr* call, const std::string& name);
    void analyzeSlotDecl(SlotDeclStmt* stmt);
    void analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn);
    bool constrain(const TypePtr& actual, const TypePtr& expected,
                   const std::string& context);
    void declareFunction(FunctionDecl* decl);
    TypePtr declaredType(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    void defineConst(const std::string& name, const ConstValue& value);
    void enterConstScope();
    void enterSlotScope();
    void error(const std::string& message, int line = 0, int column = 0);
    std::optional<ConstValue> evaluateConstExpr(
        Expr* expr,
        const std::unordered_map<std::string, ConstValue>& locals = {});
    std::optional<bool> evaluateConstraint(
        const std::string& name, const TypeVec& arguments,
        std::vector<std::string>& active);
    std::optional<ConstValue> evaluateConstraintExpr(
        Expr* expr,
        const std::unordered_map<std::string, TypePtr>& bindings,
        std::vector<std::string>& active);
    std::optional<std::string> evaluateSelectorFunction(
        FunctionDecl* function,
        const luna::selector::SymbolSet& symbols,
        const std::vector<ConstValue>& arguments,
        std::string& failure);
    void exitConstScope();
    void exitSlotScope();
    TypePtr instantiateNominal(
        const TypePtr& type, const std::vector<TypePtr>& arguments);
    SymbolInfo* lookupSymbol(const std::string& name);
    FunctionDecl* monomorphize(
        FunctionDecl* generic, const TypeVec& concreteTypes);
    void recordDeclarationReference(
        const ASTNode* source, size_t byteLength, const Decl* declaration);
    void recordResolvedReference(
        const std::string& sourcePath, int line, int column,
        size_t byteLength, std::string targetLinkageName);
    void requireBool(const TypePtr& type, const std::string& context);
    void requireInteger(const TypePtr& type, const std::string& context);
    void requireNumeric(const TypePtr& type, const std::string& context);
    TypePtr resolveTypeAST(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    TypePtr resolved(const TypePtr& type);
    bool satisfiesTrait(
        const std::string& traitId, const TypePtr& type) const;
    void setDeclarationContext(const Decl* declaration);
    void setDiagnosticLocation(const ASTNode* node);
    std::string sourceDeclarationKey(
        const std::string& name, bool diagnoseVisibility = true);
    std::string traitIdentity(const TraitDecl* trait) const;
    std::string typeIdentity(const TypePtr& type) const;
    std::unique_ptr<TypeAST> typeToAST(const TypePtr& type);
};

class DeclarationContextAccess final {
    friend class SemanticContext;

    explicit DeclarationContextAccess(SemanticContext& context);
    SemanticContext& mOwner;

public:
    using FromConversion = SemanticContext::FromConversion;
    using FromIteratorImplementation =
        SemanticContext::FromIteratorImplementation;

    decltype(SemanticContext::mConcepts)& mConcepts;
    decltype(SemanticContext::mConstexprFunctions)& mConstexprFunctions;
    decltype(SemanticContext::mCurrentModulePath)& mCurrentModulePath;
    decltype(SemanticContext::mCurrentPackageId)& mCurrentPackageId;
    decltype(SemanticContext::mDeclaredTypes)& mDeclaredTypes;
    decltype(SemanticContext::mFragments)& mFragments;
    decltype(SemanticContext::mFromConversions)& mFromConversions;
    decltype(SemanticContext::mFromIteratorImplementations)&
        mFromIteratorImplementations;
    decltype(SemanticContext::mFunctionFamilies)& mFunctionFamilies;
    decltype(SemanticContext::mImpls)& mImpls;
    decltype(SemanticContext::mMetadataSchemas)& mMetadataSchemas;
    decltype(SemanticContext::mProgram)& mProgram;
    decltype(SemanticContext::mSymTable)& mSymTable;
    decltype(SemanticContext::mTraitOwners)& mTraitOwners;
    decltype(SemanticContext::mTraitTypeParams)& mTraitTypeParams;
    decltype(SemanticContext::mTraits)& mTraits;

    TypePtr analyzeExpr(Expr* expr);
    void checkUnresolved(const TypePtr& type, const std::string& context);
    bool constrain(const TypePtr& actual, const TypePtr& expected,
                   const std::string& context);
    TypePtr declaredType(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    void error(const std::string& message, int line = 0, int column = 0);
    std::optional<SemanticConstValue> evaluateConstExpr(
        Expr* expr,
        const std::unordered_map<std::string, SemanticConstValue>& locals = {});
    std::string resolveTraitRef(TraitRef& trait, const ASTNode* useSite);
    TypePtr resolveTypeAST(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    TypePtr resolved(const TypePtr& type);
    std::string sourceDeclarationKey(
        const std::string& name, bool diagnoseVisibility = true);
    std::string traitIdentity(const TraitDecl* trait) const;
    std::string typeIdentity(const TypePtr& type) const;
};

class ControlContextAccess final {
    friend class SemanticContext;

    explicit ControlContextAccess(SemanticContext& context);
    SemanticContext& mOwner;

public:
    using SlotInfo = SemanticContext::SlotInfo;

    decltype(SemanticContext::mApplyScopes)& mApplyScopes;
    decltype(SemanticContext::mCurrentFragmentDecl)& mCurrentFragmentDecl;
    decltype(SemanticContext::mCurrentFragmentSlot)& mCurrentFragmentSlot;
    decltype(SemanticContext::mCurrentReturnType)& mCurrentReturnType;
    decltype(SemanticContext::mDynamicApplyScopes)& mDynamicApplyScopes;
    decltype(SemanticContext::mFragments)& mFragments;
    decltype(SemanticContext::mSlotScopes)& mSlotScopes;
    decltype(SemanticContext::mSymTable)& mSymTable;

    TypePtr analyzeBlock(BlockStmt* block, TypePtr expectedReturn);
    TypePtr analyzeExpr(Expr* expr);
    bool constrain(const TypePtr& actual, const TypePtr& expected,
                   const std::string& context);
    TypePtr declaredType(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    void error(const std::string& message, int line = 0, int column = 0);
    std::string sourceDeclarationKey(
        const std::string& name, bool diagnoseVisibility = true);
};

class TypeContextAccess final {
    friend class SemanticContext;

    explicit TypeContextAccess(SemanticContext& context);
    SemanticContext& mOwner;

public:
    decltype(SemanticContext::mConstraints)& mConstraints;
    decltype(SemanticContext::mDeclaredTypes)& mDeclaredTypes;
    decltype(SemanticContext::mDiagnosticCol)& mDiagnosticCol;
    decltype(SemanticContext::mDiagnosticFile)& mDiagnosticFile;
    decltype(SemanticContext::mDiagnosticLine)& mDiagnosticLine;
    decltype(SemanticContext::mGeneratedInstances)& mGeneratedInstances;
    decltype(SemanticContext::mImpls)& mImpls;
    decltype(SemanticContext::mInstantiatedFunctions)&
        mInstantiatedFunctions;
    decltype(SemanticContext::mInstantiator)& mInstantiator;
    decltype(SemanticContext::mProgram)& mProgram;
    decltype(SemanticContext::mQualifiedDeclarations)&
        mQualifiedDeclarations;
    decltype(SemanticContext::mSymTable)& mSymTable;

    void error(const std::string& message, int line = 0, int column = 0);
    TypePtr lookupDeclaredType(const std::string& name);
    void recordDeclarationReference(
        const ASTNode* source, size_t byteLength, const Decl* declaration);
    std::string sourceDeclarationKey(
        const std::string& name, bool diagnoseVisibility = true);
    std::string typeIdentity(const TypePtr& type) const;
};

class CompileTimeContextAccess final {
    friend class SemanticContext;

    explicit CompileTimeContextAccess(SemanticContext& context);
    SemanticContext& mOwner;

public:
    decltype(SemanticContext::mActiveSelectorSet)& mActiveSelectorSet;
    decltype(SemanticContext::mConcepts)& mConcepts;
    decltype(SemanticContext::mConstEvaluationDepth)& mConstEvaluationDepth;
    decltype(SemanticContext::mConstScopes)& mConstScopes;
    decltype(SemanticContext::mConstexprFunctions)& mConstexprFunctions;
    decltype(SemanticContext::mCurrentModulePath)& mCurrentModulePath;
    decltype(SemanticContext::mCurrentPackageId)& mCurrentPackageId;
    decltype(SemanticContext::mFunctionFamilies)& mFunctionFamilies;
    decltype(SemanticContext::mMetadataSchemas)& mMetadataSchemas;
    decltype(SemanticContext::mProgram)& mProgram;
    decltype(SemanticContext::mSymTable)& mSymTable;

    TypePtr analyzeExpr(Expr* expr);
    void error(const std::string& message, int line = 0, int column = 0);
    TypePtr resolveTypeAST(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings);
    TypePtr resolved(const TypePtr& type);
    void setDeclarationContext(const Decl* declaration);
    std::string sourceDeclarationKey(
        const std::string& name, bool diagnoseVisibility = true);
};
