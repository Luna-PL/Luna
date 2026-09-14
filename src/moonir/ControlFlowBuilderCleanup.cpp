#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

namespace {

constexpr const char* kCleanupShadowMarker = "$cleanup-shadow$";

std::pair<std::string, size_t> decodeCleanupPlace(const std::string& place) {
    const size_t marker = place.rfind(kCleanupShadowMarker);
    if (marker == std::string::npos) return {place, 0};
    const std::string ordinal = place.substr(
        marker + std::char_traits<char>::length(kCleanupShadowMarker));
    if (ordinal.empty()) return {place, 0};
    size_t value = 0;
    for (const unsigned char c : ordinal) {
        if (c < '0' || c > '9') return {place, 0};
        const size_t digit = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
            return {place, 0};
        value = value * 10 + digit;
    }
    return {place.substr(0, marker), value};
}

} // namespace

std::vector<CleanupId> ControlFlowBuilder::lowerCleanupObligations(
    const std::vector<CleanupObligation>& obligations,
    ScopeId sourceScope) {
    std::vector<CleanupId> result;
    for (const auto& obligation : obligations) {
        const auto [cleanupName, shadowOrdinal] =
            decodeCleanupPlace(obligation.place);
        LocalId local = lookupCleanupLocal(cleanupName, shadowOrdinal);
        if (local.empty()) {
            // A cleanup obligation for a materialized iterator binding has
            // no canonical local (the binding is registered in
            // mMaterializedIterators, not mBindings). Its cleanup is handled
            // by the source/index/limit state locals; skip the obligation.
            // Walk the full binding stack, not just the innermost level: the
            // recipe may be registered in an outer scope while a return or
            // abort inside a nested loop lowers its obligation.
            if (mGuardedConsumingRecipeNames.count(cleanupName))
                continue;
            if (const auto* recipe =
                    lookupMaterializedIterator(cleanupName)) {
                if (!recipe->sourceTailCleanups.empty()) {
                    result.insert(
                        result.end(), recipe->sourceTailCleanups.begin(),
                        recipe->sourceTailCleanups.end());
                } else if (recipe->ownsSource) {
                    const auto* sourceType =
                        mModule->findType(recipe->sourceType);
                    if (sourceType &&
                        sourceType->sysmeta.resource.cleanupRequired) {
                        const CleanupId cleanup = addCleanup(
                            recipe->source, recipe->sourceType,
                            sourceType->sysmeta.resource.cleanup,
                            CleanupKind::Value);
                        if (!cleanup.empty()) result.push_back(cleanup);
                    }
                }
                continue;
            }
            // A cleanup obligation for a captured binding has no canonical
            // local (the capture was rewritten to an EnvLoad). The closure
            // value (environment parameter) owns the cleanup, so redirect
            // to the environment parameter's cleanup (C016 CL010).
            if (!mCaptureEnvLocal.empty() &&
                std::find(mCaptureNames.begin(), mCaptureNames.end(),
                          obligation.place) != mCaptureNames.end()) {
                const auto* closureType = mModule->findType(
                    mGraph->locals[mCaptureEnvLocal.value].type);
                if (closureType &&
                    closureType->sysmeta.resource.cleanupRequired) {
                    const CleanupId cleanup = addCleanup(
                        mCaptureEnvLocal,
                        mGraph->locals[mCaptureEnvLocal.value].type,
                        closureType->sysmeta.resource.cleanup,
                        CleanupKind::Value);
                    if (!cleanup.empty()) result.push_back(cleanup);
                }
                continue;
            }
            error({}, "cleanup for '" + obligation.place +
                      "' has no canonical local");
            continue;
        }
        const TypeRef type = obligation.typeId.empty()
            ? mGraph->locals[local.value].type : obligation.typeId;
        const auto* cleanupType = mModule->findType(type);
        const CleanupKind cleanupKind =
            cleanupType && !cleanupType->sysmeta.resource.cleanupRequired &&
                obligation.action ==
                    luna::ownership::CleanupAction::Deallocate
            ? CleanupKind::Allocation
            : CleanupKind::Value;
        const CleanupId cleanup = addCleanup(
            local, type, obligation.action, cleanupKind);
        if (!cleanup.empty()) result.push_back(cleanup);
    }
    return canonicalCleanupOrder(result, sourceScope, std::nullopt);
}

std::vector<CleanupId> ControlFlowBuilder::canonicalCleanupOrder(
    const std::vector<CleanupId>& active, ScopeId sourceScope,
    std::optional<ScopeId> targetScope) const {
    std::unordered_set<uint32_t> activeSet;
    for (const auto id : active)
        if (!id.empty()) activeSet.insert(id.value);
    std::unordered_set<uint32_t> targetAncestors;
    if (targetScope) {
        for (const ScopeRecord* scope = mGraph->findScope(*targetScope); scope;
             scope = mGraph->findScope(scope->parent))
            targetAncestors.insert(scope->id.value);
    }
    std::vector<CleanupId> result;
    const auto normalizeGuardedArrayTail = [this, &result](size_t begin) {
        size_t groupBegin = begin;
        while (groupBegin < result.size()) {
            const auto* first = mGraph->findCleanup(result[groupBegin]);
            const bool isGuardedElement = first && first->guard &&
                first->place.projections.size() == 1 &&
                first->place.projections.front().kind ==
                    ProjectionKind::ConstantIndex &&
                first->place.projections.front().index ==
                    first->guard->elementIndex;
            if (!isGuardedElement) {
                ++groupBegin;
                continue;
            }
            size_t groupEnd = groupBegin + 1;
            uint64_t previousIndex = first->guard->elementIndex;
            while (groupEnd < result.size()) {
                const auto* next = mGraph->findCleanup(result[groupEnd]);
                if (!next || !next->guard ||
                    next->place.root != first->place.root ||
                    next->guard->nextUnread != first->guard->nextUnread ||
                    next->place.projections.size() != 1 ||
                    next->place.projections.front().kind !=
                        ProjectionKind::ConstantIndex ||
                    next->place.projections.front().index !=
                        next->guard->elementIndex ||
                    next->guard->elementIndex + 1 != previousIndex)
                    break;
                previousIndex = next->guard->elementIndex;
                ++groupEnd;
            }
            // Locals leave a scope in reverse declaration order, but the
            // projected elements of one owning array follow ArrayDrop's
            // source order. The guarded rows are registered by ascending
            // element index, so the reverse scope walk must restore that
            // order within this single aggregate cleanup group.
            std::reverse(
                result.begin() + static_cast<std::ptrdiff_t>(groupBegin),
                result.begin() + static_cast<std::ptrdiff_t>(groupEnd));
            groupBegin = groupEnd;
        }
    };
    for (const ScopeRecord* scope = mGraph->findScope(sourceScope); scope;
         scope = mGraph->findScope(scope->parent)) {
        if (targetAncestors.count(scope->id.value)) break;
        const size_t scopeBegin = result.size();
        for (auto cleanup = scope->cleanups.rbegin();
             cleanup != scope->cleanups.rend(); ++cleanup)
            if (activeSet.count(cleanup->value))
                result.push_back(*cleanup);
        normalizeGuardedArrayTail(scopeBegin);
    }
    return result;
}

void ControlFlowBuilder::canonicalizeCleanupTable() {
    std::vector<uint32_t> order(mGraph->cleanups.size());
    for (uint32_t index = 0; index < order.size(); ++index) order[index] = index;
    const auto projectionLess = [](
        const PlaceProjection& left, const PlaceProjection& right) {
        if (left.kind != right.kind)
            return static_cast<uint8_t>(left.kind) <
                static_cast<uint8_t>(right.kind);
        if (left.index != right.index) return left.index < right.index;
        return left.dynamicIndex.value < right.dynamicIndex.value;
    };
    std::sort(order.begin(), order.end(),
              [this, &projectionLess](uint32_t lhs, uint32_t rhs) {
        const auto& left = mGraph->cleanups[lhs];
        const auto& right = mGraph->cleanups[rhs];
        if (left.scope != right.scope) return left.scope.value < right.scope.value;
        if (left.place.root != right.place.root)
            return left.place.root.value < right.place.root.value;
        if (left.place.projections != right.place.projections)
            return std::lexicographical_compare(
                left.place.projections.begin(),
                left.place.projections.end(),
                right.place.projections.begin(),
                right.place.projections.end(), projectionLess);
        if (left.kind != right.kind)
            return static_cast<uint8_t>(left.kind) <
                static_cast<uint8_t>(right.kind);
        if (left.action != right.action)
            return static_cast<uint8_t>(left.action) <
                static_cast<uint8_t>(right.action);
        if (left.type != right.type)
            return left.type.value < right.type.value;
        if (left.guard.has_value() != right.guard.has_value())
            return !left.guard.has_value();
        if (left.guard && right.guard) {
            if (left.guard->nextUnread != right.guard->nextUnread)
                return left.guard->nextUnread.value <
                    right.guard->nextUnread.value;
            if (left.guard->elementIndex != right.guard->elementIndex)
                return left.guard->elementIndex <
                    right.guard->elementIndex;
        }
        return lhs < rhs;
    });
    std::vector<uint32_t> remap(order.size());
    std::vector<CleanupRecord> canonical;
    canonical.reserve(order.size());
    for (uint32_t newIndex = 0; newIndex < order.size(); ++newIndex) {
        remap[order[newIndex]] = newIndex;
        auto record = std::move(mGraph->cleanups[order[newIndex]]);
        record.id = CleanupId{newIndex};
        canonical.push_back(std::move(record));
    }
    mGraph->cleanups = std::move(canonical);
    const auto rewrite = [&remap](std::vector<CleanupId>& references) {
        for (auto& reference : references)
            if (!reference.empty() && reference.value < remap.size())
                reference.value = remap[reference.value];
    };
    for (auto& scope : mGraph->scopes) {
        rewrite(scope.cleanups);
        std::sort(scope.cleanups.begin(), scope.cleanups.end(),
                  [](CleanupId lhs, CleanupId rhs) {
            return lhs.value < rhs.value;
        });
    }
    for (auto& block : mGraph->blocks) {
        rewrite(block.terminator.primary.cleanups);
        rewrite(block.terminator.secondary.cleanups);
        rewrite(block.terminator.exitCleanups);
        for (auto& item : block.terminator.cases)
            rewrite(item.edge.cleanups);
    }
}

void ControlFlowBuilder::error(
    const SourceLocation& location, const std::string& message) {
    if (location.path.empty()) {
        mErrors.push_back(message);
        return;
    }
    mErrors.push_back(location.path + ":" + std::to_string(location.line) +
                      ":" + std::to_string(location.column) + ": " + message);
}

} // namespace moon
