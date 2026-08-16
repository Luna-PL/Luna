#pragma once

#include "diagnostics/Diagnostic.h"

#include "TypeSystem.h"
#include "SymbolTable.h"
#include "../parser/AST.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>

class OwnershipChecker {
public:
    OwnershipChecker();

    bool check(Program* program, SymbolTable& symTable);
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    enum class OwnState { Valid, Moved, Freed };

    // A Place identifies storage, not merely a value-producing expression.
    // Components are field names, constant indices, `[*]`, or `*` for a
    // dereference. Prefix overlap is the basis for partial-move and loan
    // conflict checks.
    struct Place {
        std::string root;
        std::vector<std::string> components;
    };

    struct Loan {
        Place source;
        bool isMutable = false;
    };

    struct VarInfo {
        OwnState state = OwnState::Valid;
        TypePtr type;
        bool isHeapAllocated = false;
        luna::ownership::Usage usage = luna::ownership::Usage::Copy;
        luna::ownership::Relation relation = luna::ownership::Relation::Owned;
        std::vector<Place> movedPlaces;
        bool isReference = false;
        bool isMutableReference = false;
        int sharedBorrows = 0;
        bool mutableBorrow = false;
        int inFlightReads = 0;
        int inFlightWrites = 0;
        bool isGpuEvent = false;
        bool materializedIteratorOwnsSource = false;
        TypePtr materializedIteratorSourceType;
        std::vector<Loan> eventResources;
        std::string name;
    };

    // `ok` reports whether ownership checking succeeded.  `fallsThrough`
    // distinguishes a normal continuation from a path terminated by return.
    // Keeping this separate is essential: a resource consumed by a returning
    // branch must not be merged with the state of code that remains reachable.
    struct FlowResult {
        bool ok;
        bool fallsThrough;

        FlowResult(bool succeeded = true, bool continues = true)
            : ok(succeeded), fallsThrough(continues) {}
    };

    struct CheckerState {
        std::vector<std::unordered_map<std::string, VarInfo>> scopes;
        std::vector<std::vector<Loan>> loans;
        std::vector<std::unordered_map<std::string, FragmentDecl*>> applyScopes;
        std::vector<std::unordered_map<std::string, SlotDeclStmt*>> slotScopes;
    };

    bool checkFunction(FunctionDecl* decl);
    bool checkLambda(LambdaExpr* lambda);
    FlowResult checkBlock(BlockStmt* block);
    FlowResult checkStmt(Stmt* stmt);
    bool checkExpr(Expr* expr);
    // Large checkStmt branches split out for readability; behavior unchanged.
    FlowResult checkLetStmt(LetStmt* stmt);
    FlowResult checkForStmt(ForStmt* stmt);
    FlowResult checkReturnStmt(ReturnStmt* stmt);
    FlowResult checkMatchStmt(MatchStmt* stmt);
    FlowResult checkAbortStmt(AbortStmt* stmt);
    // Large checkExpr branches split out for readability.
    bool checkCallExpr(CallExpr* call);
    bool checkVariantConstruct(VariantConstructExpr* expr);
    bool checkRecordLiteral(RecordLiteralExpr* expr);
    bool checkLaunchExpr(LaunchExpr* launch);
    FlowResult checkSlotInvoke(SlotInvokeStmt* slot);
    FlowResult checkFragment(FragmentDecl* fragment, SlotInvokeStmt* slot, bool multiShot);
    bool continuationConsumesCapturedState(const std::vector<std::unordered_map<std::string, VarInfo>>& before) const;
    CheckerState captureState() const;
    void restoreState(const CheckerState& state);
    bool mergeFallthroughStates(const CheckerState& before,
                                const CheckerState& left,
                                const CheckerState& right,
                                const ASTNode* controlFlow);
    bool loopPreservesOuterState(const CheckerState& before,
                                 const CheckerState& after,
                                 const ASTNode* loop);
    bool sameVarState(const VarInfo& left, const VarInfo& right) const;
    bool sameLoanState(const std::vector<Loan>& left, const std::vector<Loan>& right) const;
    bool sameApplyState(const std::vector<std::unordered_map<std::string, FragmentDecl*>>& left,
                        const std::vector<std::unordered_map<std::string, FragmentDecl*>>& right) const;
    bool sameSlotState(const std::vector<std::unordered_map<std::string, SlotDeclStmt*>>& left,
                       const std::vector<std::unordered_map<std::string, SlotDeclStmt*>>& right) const;
    std::string describeControlFlowDifference(const std::string& name,
                                              const VarInfo& left,
                                              const VarInfo& right,
                                              const char* construct) const;

    void enterScope();
    void exitScope();
    void releaseLoansInCurrentScope();
    bool acquireLoan(const Place& place, bool isMutable);
    bool beginInFlightBorrow(const std::string& name, bool isMutable);
    void finishEvent(VarInfo* event);
    void releaseLoan(const Loan& loan);
    bool consume(const Place& place, const std::string& action);
    bool consume(VarInfo* var, const std::string& action);
    bool checkWriteTarget(Expr* expr);
    luna::ownership::Usage usageFromTypeAST(const TypeAST* ast) const;
    bool isReferenceExpr(Expr* expr);
    bool isDeviceBuffer(const TypePtr& type) const;
    bool isEvent(const TypePtr& type) const;
    std::optional<Place> extractPlace(Expr* expr) const;
    std::string renderPlace(const Place& place) const;
    std::string renderProjection(const Place& place) const;
    bool placesOverlap(const Place& left, const Place& right) const;
    bool isPlaceAvailable(const Place& place, const std::string& action);
    bool hasConflictingLoan(const Place& place, bool forMutation) const;
    bool allDirectFieldsMoved(const VarInfo& var) const;
    TypePtr typeOfPlace(const Place& place) const;

    VarInfo* lookup(const std::string& name);
    void define(const std::string& name, TypePtr type, bool isHeap,
                luna::ownership::Usage usage = luna::ownership::Usage::Copy,
                luna::ownership::Relation relation = luna::ownership::Relation::Owned,
                bool isReference = false, bool isMutableReference = false);

    std::vector<std::string> collectFreesAtScopeExit();
    std::vector<std::string> collectFreesAtReturn() const;
    std::vector<std::string> collectFreesAtFragmentExit() const;
    void validateLinearScope();
    void validateLinearReturnPath();
    void error(const std::string& msg, int line = 0, int col = 0);
    void setDiagnosticLocation(const ASTNode* node);

    std::vector<std::unordered_map<std::string, VarInfo>> mScopes;
    // Loans belong to the lexical scope in which the reference was created.
    // This gives references a lexical lifetime and releases them before the
    // source variable can be automatically freed.
    std::vector<std::vector<Loan>> mLoansInScope;
    std::vector<diagnostic::Diagnostic> mErrors;
    SymbolTable* mSymTable = nullptr;
    std::string mDiagnosticFile;
    int mDiagnosticLine = 0;
    int mDiagnosticCol = 0;
    std::unordered_map<std::string, FragmentDecl*> mFragments;
    std::vector<std::unordered_map<std::string, FragmentDecl*>> mApplyScopes;
    std::vector<std::unordered_map<std::string, SlotDeclStmt*>> mSlotScopes;
    BlockStmt* mCurrentSlotContinuation = nullptr;
    bool mValidatingManyContinuation = false;
    bool mCheckingSlotContinuation = false;
    std::vector<CheckerState>* mCurrentFragmentAbortExits = nullptr;
    size_t mCurrentFragmentScopeBase = 0;
    size_t mCurrentFragmentApplyBase = 0;
    size_t mCurrentFragmentSlotBase = 0;
    // While checking a lambda, Copy locals from enclosing scopes are kept
    // here as capture candidates: they are registered as value copies in the
    // lambda scope so the body reads the captured copy. Affine/Linear entries
    // are diagnosed (C016 CL005); borrowed captures are diagnosed too.
    std::unordered_map<std::string, luna::ownership::Usage>
        mUnavailableLambdaCaptures;
};
