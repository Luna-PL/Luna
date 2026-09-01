#pragma once

#include "SemanticAnalyzer.h"
#include "SymbolTable.h"
#include "TypeSystem.h"
#include "../instantiation/Instantiator.h"
#include "../parser/AST.h"

#include <optional>
#include <memory>
#include <utility>
#include <variant>

namespace luna::selector {
class SymbolCatalog;
class SymbolSet;
}

class BodyContextAccess;
class CompileTimeContextAccess;
class ControlContextAccess;
class DeclarationContextAccess;
class TypeContextAccess;

using SemanticConstValue =
    std::variant<int64_t, double, bool, std::string>;

struct SemanticSelectorDeclarationValue {
    std::string declarationId;
};

struct SemanticSelectorMetadataValue {
    std::string schemaId;
    std::vector<SemanticConstValue> fields;
};

struct SemanticSelectorDeclarationViewValue {
    std::vector<std::string> declarationIds;
};

struct SemanticSelectorMetadataViewValue {
    std::vector<SemanticSelectorMetadataValue> values;
};

using SemanticSelectorValue = std::variant<
    int64_t, double, bool, std::string,
    SemanticSelectorDeclarationValue, SemanticSelectorMetadataValue,
    SemanticSelectorDeclarationViewValue, SemanticSelectorMetadataViewValue>;

class BodyAnalysis {
public:
    virtual ~BodyAnalysis() = default;

    virtual void analyzeFunction(FunctionDecl* decl) = 0;
    virtual void analyzeStruct(StructDecl* decl) = 0;
    virtual void analyzeEnum(EnumDecl* decl) = 0;
    virtual void analyzeTrait(TraitDecl* decl) = 0;
    virtual void analyzeImpl(ImplDecl* decl) = 0;
    virtual TypePtr analyzeStmt(Stmt* stmt, TypePtr expectedReturn) = 0;
    virtual TypePtr analyzeBlock(
        BlockStmt* block, TypePtr expectedReturn) = 0;
    virtual TypePtr analyzeExpr(Expr* expr) = 0;
    virtual TypePtr analyzeCall(CallExpr* call) = 0;
    virtual TypePtr analyzeMemberCall(
        CallExpr* call, FieldAccessExpr* member) = 0;
    virtual TypePtr analyzeIteratorCall(
        CallExpr* call, FieldAccessExpr* member) = 0;
    virtual TypePtr analyzeLaunch(LaunchExpr* launch) = 0;
    virtual TypePtr analyzeSelect(SelectExpr* selection) = 0;
    virtual bool statementAlwaysReturns(const Stmt* stmt) const = 0;
    virtual bool blockAlwaysReturns(const BlockStmt* block) const = 0;
};

class TypeAnalysis {
public:
    virtual ~TypeAnalysis() = default;

    virtual FunctionDecl* findMatchingImpl(
        const std::string& traitName, const std::string& typeName,
        const std::string& methodName) = 0;
    virtual FunctionDecl* monomorphize(
        FunctionDecl* generic, const TypeVec& concreteTypes) = 0;
    virtual TypePtr resolveTypeAST(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings) = 0;
    virtual TypePtr instantiateNominal(
        const TypePtr& type, const std::vector<TypePtr>& args) = 0;
    virtual TypePtr declaredType(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings) = 0;
    virtual TypePtr resolved(const TypePtr& type) = 0;
    virtual bool constrain(
        const TypePtr& actual, const TypePtr& expected,
        const std::string& context) = 0;
    virtual void requireBool(
        const TypePtr& type, const std::string& context) = 0;
    virtual void requireNumeric(
        const TypePtr& type, const std::string& context) = 0;
    virtual void requireInteger(
        const TypePtr& type, const std::string& context) = 0;
    virtual void checkUnresolved(
        const TypePtr& type, const std::string& context) = 0;
    virtual std::unique_ptr<TypeAST> typeToAST(const TypePtr& type) = 0;
    virtual void materializeInferredTypes(Program* program) = 0;
};

class CompileTimeAnalysis {
public:
    virtual ~CompileTimeAnalysis() = default;

    virtual TypePtr analyzeReflectionCall(
        CallExpr* call, const std::string& name) = 0;
    virtual TypePtr analyzeDeclarationReflectionCall(
        CallExpr* call, const std::string& name) = 0;
    virtual void enterConstScope() = 0;
    virtual void exitConstScope() = 0;
    virtual void defineConst(
        const std::string& name, const SemanticConstValue& value) = 0;
    virtual const SemanticConstValue* lookupConst(
        const std::string& name) const = 0;
    virtual std::optional<SemanticConstValue> evaluateConstExpr(
        Expr* expr,
        const std::unordered_map<std::string, SemanticConstValue>& locals) = 0;
    virtual std::optional<SemanticConstValue> evaluateConstFunction(
        FunctionDecl* function,
        const std::vector<SemanticConstValue>& arguments) = 0;
    virtual bool evaluateConstBlock(
        BlockStmt* block,
        std::unordered_map<std::string, SemanticConstValue>& locals,
        std::optional<SemanticConstValue>& result) = 0;
    virtual std::optional<SemanticConstValue> evaluateConstraintExpr(
        Expr* expr,
        const std::unordered_map<std::string, TypePtr>& bindings,
        std::vector<std::string>& active) = 0;
    virtual std::optional<bool> evaluateConstraint(
        const std::string& name, const TypeVec& arguments,
        std::vector<std::string>& active) = 0;
    virtual std::optional<SemanticSelectorValue> evaluateSelectorExpr(
        Expr* expr,
        std::unordered_map<std::string, SemanticSelectorValue>& locals) = 0;
    virtual bool evaluateSelectorBlock(
        BlockStmt* block,
        std::unordered_map<std::string, SemanticSelectorValue>& locals,
        std::optional<SemanticSelectorValue>& result,
        bool& returned) = 0;
    virtual std::optional<std::string> evaluateSelectorFunction(
        FunctionDecl* function,
        const luna::selector::SymbolSet& symbols,
        const std::vector<SemanticConstValue>& arguments,
        std::string& failure) = 0;
};

class DeclarationAnalysis {
public:
    virtual ~DeclarationAnalysis() = default;

    virtual void declareFunction(FunctionDecl* decl) = 0;
    virtual void declareMeta(MetaDecl* decl) = 0;
    virtual void declareConstraint(ConstraintDecl* decl) = 0;
    virtual void analyzeConstraint(ConstraintDecl* decl) = 0;
    virtual void analyzeMeta(MetaDecl* decl) = 0;
    virtual void validateMetadata(Decl* decl) = 0;
    virtual void declareFragment(FragmentDecl* decl) = 0;
    virtual bool isFFIType(
        const TypePtr& type, const std::string& context) = 0;
    virtual void validateFFIFunction(FunctionDecl* decl) = 0;
    virtual void declareStruct(StructDecl* decl) = 0;
    virtual void declareEnum(EnumDecl* decl) = 0;
    virtual void declareTrait(TraitDecl* decl) = 0;
    virtual void declareImpl(ImplDecl* decl) = 0;
};

class ControlAnalysis {
public:
    virtual ~ControlAnalysis() = default;

    virtual void declareSlot(SlotDecl* decl) = 0;
    virtual void finalizeSlot(SlotDecl* decl) = 0;
    virtual void analyzeSlotDecl(SlotDeclStmt* stmt) = 0;
    virtual void analyzeSlotInvoke(
        SlotInvokeStmt* stmt, TypePtr expectedReturn) = 0;
    virtual void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) = 0;
    virtual void analyzeFragmentForSlot(
        FragmentDecl* fragment, const std::string& slotName,
        const TypeVec& parameterTypes,
        const std::vector<luna::ownership::Contract>& parameterContracts,
        const std::unordered_map<std::string, SymbolInfo>& captures) = 0;
    virtual void enterSlotScope() = 0;
    virtual void exitSlotScope() = 0;
    virtual FragmentDecl* selectFragment(
        const std::string& name, const ASTNode* useSite) = 0;
};

class SemanticContext {
    friend class BodyContextAccess;
    friend class CompileTimeContextAccess;
    friend class ControlContextAccess;
    friend class DeclarationContextAccess;
    friend class TypeContextAccess;

public:
    SemanticContext();

    BodyContextAccess bodyAccess();
    CompileTimeContextAccess compileTimeAccess();
    ControlContextAccess controlAccess();
    DeclarationContextAccess declarationAccess();
    TypeContextAccess typeAccess();

    void bindBodyAnalysis(BodyAnalysis& bodyAnalysis) {
        mBodyAnalysis = &bodyAnalysis;
    }

    void bindTypeAnalysis(TypeAnalysis& typeAnalysis) {
        mTypeAnalysis = &typeAnalysis;
    }

    void bindCompileTimeAnalysis(CompileTimeAnalysis& compileTimeAnalysis) {
        mCompileTimeAnalysis = &compileTimeAnalysis;
    }

    void bindDeclarationAnalysis(DeclarationAnalysis& declarationAnalysis) {
        mDeclarationAnalysis = &declarationAnalysis;
    }

    void bindControlAnalysis(ControlAnalysis& controlAnalysis) {
        mControlAnalysis = &controlAnalysis;
    }

    bool analyze(Program* program);
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }
    SymbolTable& symTable() { return mSymTable; }
    const SymbolTable& symTable() const { return mSymTable; }
    const luna::selector::SymbolCatalog* symbolCatalog() const {
        return mSymbolCatalog.get();
    }
    const std::vector<ResolvedDeclarationReference>& declarationReferences() const {
        return mDeclarationReferences;
    }

private:
    using ConstValue = SemanticConstValue;
    using SelectorDeclarationValue = SemanticSelectorDeclarationValue;
    using SelectorMetadataValue = SemanticSelectorMetadataValue;
    using SelectorDeclarationViewValue =
        SemanticSelectorDeclarationViewValue;
    using SelectorMetadataViewValue = SemanticSelectorMetadataViewValue;
    using SelectorValue = SemanticSelectorValue;

    TypePtr resolveTypeAST(const TypeAST* ast,
                           const std::unordered_map<std::string, TypePtr>& bindings);

    void declareFunction(FunctionDecl* decl);
    void declareStruct(StructDecl* decl);
    void declareEnum(EnumDecl* decl);
    void declareTrait(TraitDecl* decl);
    void declareImpl(ImplDecl* decl);
    void declareFragment(FragmentDecl* decl);
    void declareMeta(MetaDecl* decl);
    void declareConstraint(ConstraintDecl* decl);
    void validateFFIFunction(FunctionDecl* decl);
    bool isFFIType(const TypePtr& type, const std::string& context);

    void analyzeFunction(FunctionDecl* decl);
    void analyzeStruct(StructDecl* decl);
    void analyzeEnum(EnumDecl* decl);
    void analyzeTrait(TraitDecl* decl);
    void analyzeImpl(ImplDecl* decl);
    void analyzeMeta(MetaDecl* decl);
    void analyzeConstraint(ConstraintDecl* decl);
    void validateMetadata(Decl* decl);
    void rebuildSymbolCatalog();
    void analyzeSlotDecl(SlotDeclStmt* stmt);
    void declareSlot(SlotDecl* decl);
    void finalizeSlot(SlotDecl* decl);
    void analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn);
    void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn);
    void analyzeFragmentForSlot(FragmentDecl* fragment, const std::string& slotName,
                                const TypeVec& parameterTypes,
                                const std::vector<luna::ownership::Contract>& parameterContracts,
                                const std::unordered_map<std::string, SymbolInfo>& captures);

    TypePtr analyzeStmt(Stmt* stmt, TypePtr expectedReturnType);
    TypePtr analyzeBlock(BlockStmt* block, TypePtr expectedReturnType);
    TypePtr analyzeExpr(Expr* expr);
    TypePtr analyzeCall(CallExpr* call);
    TypePtr analyzeMemberCall(CallExpr* call, FieldAccessExpr* member);
    TypePtr analyzeIteratorCall(CallExpr* call, FieldAccessExpr* member);
    TypePtr analyzeLaunch(LaunchExpr* launch);
    TypePtr analyzeSelect(SelectExpr* selection);
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
    TypePtr analyzeDeclarationReflectionCall(CallExpr* call,
                                             const std::string& name);
    std::optional<SelectorValue> evaluateSelectorExpr(
        Expr* expr, std::unordered_map<std::string, SelectorValue>& locals);
    bool evaluateSelectorBlock(
        BlockStmt* block, std::unordered_map<std::string, SelectorValue>& locals,
        std::optional<SelectorValue>& result, bool& returned);
    std::optional<std::string> evaluateSelectorFunction(
        FunctionDecl* function, const luna::selector::SymbolSet& symbols,
        const std::vector<ConstValue>& arguments, std::string& failure);
    std::optional<ConstValue> evaluateConstraintExpr(
        Expr* expr, const std::unordered_map<std::string, TypePtr>& bindings,
        std::vector<std::string>& active);
    std::optional<bool> evaluateConstraint(
        const std::string& name, const TypeVec& arguments,
        std::vector<std::string>& active);
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
    FragmentDecl* selectFragment(const std::string& name,
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
    void setDeclarationContext(const Decl* declaration);
    std::string sourceDeclarationKey(const std::string& name,
                                     bool diagnoseVisibility = true);
    SymbolInfo* lookupSymbol(const std::string& name);
    TypePtr lookupDeclaredType(const std::string& name);
    void recordDeclarationReference(const ASTNode* source,
                                    size_t byteLength,
                                    const Decl* declaration);
    void recordResolvedReference(const std::string& sourcePath,
                                 int line, int column, size_t byteLength,
                                 std::string targetLinkageName);

    BodyAnalysis* mBodyAnalysis = nullptr;
    TypeAnalysis* mTypeAnalysis = nullptr;
    CompileTimeAnalysis* mCompileTimeAnalysis = nullptr;
    DeclarationAnalysis* mDeclarationAnalysis = nullptr;
    ControlAnalysis* mControlAnalysis = nullptr;
    SymbolTable mSymTable;
    std::unordered_map<std::string, MetaDecl*> mMetadataSchemas;
    std::unordered_map<std::string, ConstraintDecl*> mConcepts;
    std::unordered_map<std::string, std::vector<FunctionDecl*>> mFunctionFamilies;
    std::shared_ptr<const luna::selector::SymbolCatalog> mSymbolCatalog;
    std::unordered_map<std::string, Decl*> mQualifiedDeclarations;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> mPackageAliases;
    // Exact TraitId → (exact target TypeId → [methodName → FunctionDecl*]).
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::unordered_map<std::string, FunctionDecl*>>> mImpls;
    struct FromConversion {
        TypePtr source;
        TypePtr target;
        FunctionDecl* method = nullptr;
        std::string symbol;
    };
    // Exact target TypeId -> exact source TypeId -> one static conversion.
    std::unordered_map<std::string,
        std::unordered_map<std::string, FromConversion>> mFromConversions;
    struct FromIteratorImplementation {
        TypePtr item;
        TypePtr builder;
        TypePtr target;
        FunctionDecl* begin = nullptr;
        FunctionDecl* push = nullptr;
        FunctionDecl* finish = nullptr;
    };
    // Exact target TypeId -> one coherent Core FromIterator builder protocol.
    std::unordered_map<std::string, FromIteratorImplementation>
        mFromIteratorImplementations;
    // Trait → typeParams
    std::unordered_map<std::string, std::vector<std::string>> mTraitTypeParams;
    // Trait → methods
    std::unordered_map<std::string, std::vector<std::pair<std::string, FunctionDecl*>>> mTraitMethods;
    // Exact TraitId → owning Package ID. Compiler-known traits use the
    // reserved compiler owner and can only be implemented through their
    // stricter built-in orphan rules.
    std::unordered_map<std::string, std::string> mTraitOwners;
    std::unordered_map<std::string, TraitDecl*> mTraits;
    // Source declaration names map to resolved types regardless of whether
    // their identity policy is structural or nominal.
    std::unordered_map<std::string, TypePtr> mDeclaredTypes;

    std::vector<std::unique_ptr<FunctionDecl>> mGeneratedInstances;
    luna::instantiation::Instantiator mInstantiator;
    std::unordered_map<std::string, FunctionDecl*> mInstantiatedFunctions;
    std::vector<diagnostic::Diagnostic> mErrors;
    std::vector<ResolvedDeclarationReference> mDeclarationReferences;
    Program* mProgram = nullptr;
    std::string mCurrentPackageId;
    std::string mCurrentModulePath;
    std::string mDiagnosticFile;
    int mDiagnosticLine = 0;
    int mDiagnosticCol = 0;

    // For current function context
    TypePtr mCurrentReturnType;
    bool mInFunction = false;
    bool mInKernel = false;
    bool mCurrentFunctionReturnsLinear = false;
    luna::ownership::Usage mCurrentFunctionReturnUsage = luna::ownership::Usage::Copy;
    bool mSawReturn = false;
    ConstraintSolver mConstraints;
    std::vector<std::pair<TypePtr, std::string>> mInferenceRoots;
    std::vector<std::unordered_map<std::string, ConstValue>> mConstScopes;
    std::unordered_map<std::string, FunctionDecl*> mConstexprFunctions;
    int mConstEvaluationDepth = 0;
    uint64_t mIteratorStateCounter = 0;
    const luna::selector::SymbolSet* mActiveSelectorSet = nullptr;
    struct SlotInfo {
        SlotDecl* declaration = nullptr;
        std::string name;
        TypeVec paramTypes;
        std::vector<luna::ownership::Contract> paramContracts;
        std::vector<std::string> paramNames;
        std::string defaultFragment;
        std::string resolvedDefaultFragmentName;
        FragmentKind acceptedKind = FragmentKind::Interceptor;
        FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
        bool isImplicitCapture = false;
        TypePtr structuralType;
    };
    std::vector<std::unordered_map<std::string, SlotInfo>> mSlotScopes;
    std::vector<std::unordered_map<std::string, FragmentDecl*>> mApplyScopes;
    std::unordered_map<std::string, FragmentDecl*> mFragments;
    const SlotInfo* mCurrentFragmentSlot = nullptr;
    FragmentDecl* mCurrentFragmentDecl = nullptr;
};
