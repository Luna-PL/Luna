#include "Selector.h"

#include <unordered_set>

namespace luna::selector {

DeclarationView::DeclarationView(std::vector<Candidate> candidates)
    : mCandidates(std::move(candidates)) {
    std::unordered_set<std::string> ids;
    for (const auto& candidate : mCandidates) {
        if (candidate.declarationId.empty()) {
            mValid = false;
            mError = "selector candidate has no declaration identity";
            return;
        }
        if (!ids.insert(candidate.declarationId).second) {
            mValid = false;
            mError = "selector view contains duplicate candidate '" +
                     candidate.declarationId + "'";
            return;
        }
        if (mFamilyId.empty()) mFamilyId = candidate.familyId;
        else if (mFamilyId != candidate.familyId) {
            mValid = false;
            mError = "selector view crosses declaration family boundaries";
            return;
        }
    }
}

const Candidate* DeclarationView::find(const std::string& declarationId) const {
    for (const auto& candidate : mCandidates)
        if (candidate.declarationId == declarationId) return &candidate;
    return nullptr;
}

Result Engine::validate(const DeclarationView& view,
                        const std::vector<std::string>& returnedIds) const {
    if (!view.valid())
        return {ResultKind::InvalidView, std::nullopt, view.error()};
    if (returnedIds.empty())
        return {ResultKind::NoMatch, std::nullopt,
                "selector returned no legal declaration"};
    if (returnedIds.size() != 1)
        return {ResultKind::Ambiguous, std::nullopt,
                "selector must return exactly one declaration"};
    const auto* selected = view.find(returnedIds.front());
    if (!selected)
        return {ResultKind::InvalidCandidate, std::nullopt,
                "selector returned a declaration outside its injected candidate view"};
    return {ResultKind::Unique, *selected, {}};
}

std::optional<DynamicPlan> Engine::planDynamic(
    const DeclarationView& view, const std::string& selectorDeclarationId,
    std::string& error) const {
    error.clear();
    if (!view.valid()) {
        error = view.error();
        return std::nullopt;
    }
    if (selectorDeclarationId.empty()) {
        error = "dynamic selector has no declaration identity";
        return std::nullopt;
    }
    DynamicPlan plan;
    plan.familyId = view.familyId();
    plan.selectorDeclarationId = selectorDeclarationId;
    for (const auto& candidate : view.candidates()) {
        if (candidate.retention == Retention::CompileTime) {
            error = "dynamic select candidate '" + candidate.declarationId +
                    "' has no runtime descriptor";
            return std::nullopt;
        }
        plan.candidateIds.push_back(candidate.declarationId);
    }
    if (plan.candidateIds.empty()) {
        error = "dynamic select has an empty candidate view";
        return std::nullopt;
    }
    return plan;
}

} // namespace luna::selector
