#pragma once

#include "SemanticContextAccess.h"

class ControlAnalyzer final : public ControlAnalysis {
public:
    explicit ControlAnalyzer(ControlContextAccess context)
        : mContext(std::move(context)) {}

    void declareSlot(SlotDecl* decl) override;
    void finalizeSlot(SlotDecl* decl) override;
    void analyzeSlotDecl(SlotDeclStmt* stmt) override;
    void analyzeSlotInvoke(
        SlotInvokeStmt* stmt, TypePtr expectedReturn) override;
    void analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) override;
    void analyzeFragmentForSlot(
        FragmentDecl* fragment, const std::string& slotName,
        const TypeVec& parameterTypes,
        const std::vector<luna::ownership::Contract>& parameterContracts,
        const std::unordered_map<std::string, SymbolInfo>& captures) override;

    void enterSlotScope() override;
    void exitSlotScope() override;
    FragmentDecl* selectFragment(const std::string& name,
                                 const ASTNode* useSite) override;

private:
    ControlContextAccess mContext;
};
