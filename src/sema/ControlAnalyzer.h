#pragma once

#include "SemanticContextAccess.h"

#include <vector>

class ControlAnalyzer final : public ControlAnalysis {
public:
    explicit ControlAnalyzer(ControlContextAccess context)
        : mContext(std::move(context)) {}

    void declareSlot(SlotDecl* decl) override;
    void finalizeSlot(SlotDecl* decl) override;
    void analyzeSlotInvoke(
        SlotInvokeStmt* stmt, TypePtr expectedReturn) override;
    void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) override;
    void analyzeFragmentForSlot(
        FragmentDecl* fragment, const std::string& slotName,
        const TypeVec& parameterTypes,
        const std::vector<luna::ownership::Contract>& parameterContracts) override;

    void enterSlotScope() override;
    void exitSlotScope() override;
    FragmentDecl* selectFragment(const std::string& name,
                                 const ASTNode* useSite) override;

private:
    ControlContextAccess mContext;
    // Tracks construction-time body expansion, not runtime activations.
    std::vector<FragmentDecl*> mAnalyzingFragments;
};
