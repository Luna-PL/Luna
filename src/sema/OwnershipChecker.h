#pragma once

#include "TypeSystem.h"
#include "SymbolTable.h"
#include "../parser/AST.h"
#include <string>
#include <vector>
#include <unordered_map>

class OwnershipChecker {
public:
    OwnershipChecker();

    bool check(Program* program, SymbolTable& symTable);
    const std::vector<std::string>& errors() const { return mErrors; }

private:
    enum class OwnState { Valid, Moved, Freed };

    struct Loan {
        std::string source;
        bool isMutable = false;
    };

    struct VarInfo {
        OwnState state = OwnState::Valid;
        TypePtr type;
        bool isHeapAllocated = false;
        bool isLinear = false;
        bool isReference = false;
        bool isMutableReference = false;
        int sharedBorrows = 0;
        bool mutableBorrow = false;
        int inFlightReads = 0;
        int inFlightWrites = 0;
        bool isGpuEvent = false;
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
    FlowResult checkBlock(BlockStmt* block);
    FlowResult checkStmt(Stmt* stmt);
    bool checkExpr(Expr* expr);
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
    bool acquireLoan(const std::string& name, bool isMutable);
    bool beginInFlightBorrow(const std::string& name, bool isMutable);
    void finishEvent(VarInfo* event);
    void releaseLoan(const Loan& loan);
    bool consume(VarInfo* var, const std::string& action);
    bool checkWriteTarget(Expr* expr);
    bool isLinearTypeAST(const TypeAST* ast) const;
    bool isReferenceExpr(Expr* expr);
    bool isDeviceBuffer(const TypePtr& type) const;
    bool isEvent(const TypePtr& type) const;

    VarInfo* lookup(const std::string& name);
    void define(const std::string& name, TypePtr type, bool isHeap,
                bool isLinear = false, bool isReference = false,
                bool isMutableReference = false);

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
    std::vector<std::string> mErrors;
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
};
