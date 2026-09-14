#include "OwnershipChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <functional>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace {

constexpr const char* kCleanupShadowMarker = "$cleanup-shadow$";

std::string cleanupPlace(const std::string& name, size_t shadowOrdinal) {
    if (shadowOrdinal == 0) return name;
    return name + kCleanupShadowMarker + std::to_string(shadowOrdinal);
}

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

void OwnershipChecker::enterScope() {
    mScopes.emplace_back();
    mLoansInScope.emplace_back();
}

void OwnershipChecker::exitScope() {
    releaseLoansInCurrentScope();
    mLoansInScope.pop_back();
    if (mScopes.size() > 1) mScopes.pop_back();
}

void OwnershipChecker::releaseLoansInCurrentScope() {
    auto& loans = mLoansInScope.back();
    while (!loans.empty()) {
        releaseLoan(loans.back());
        loans.pop_back();
    }
}

std::optional<OwnershipChecker::Place> OwnershipChecker::extractPlace(Expr* expr) const {
    if (!expr) return std::nullopt;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) return Place{id->name, {}};
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr)) {
        auto place = extractPlace(field->object.get());
        if (!place) return std::nullopt;
        place->components.push_back("." + field->field);
        return place;
    }
    if (auto* index = dynamic_cast<IndexExpr*>(expr)) {
        auto place = extractPlace(index->object.get());
        if (!place) return std::nullopt;
        if (auto* literal = dynamic_cast<IntLiteralExpr*>(index->index.get()))
            place->components.push_back("[" + std::to_string(literal->value) + "]");
        else
            place->components.push_back("[*]");
        return place;
    }
    if (auto* dereference = dynamic_cast<DerefExpr*>(expr)) {
        auto place = extractPlace(dereference->operand.get());
        if (!place) return std::nullopt;
        place->components.push_back("*");
        return place;
    }
    return std::nullopt;
}

std::string OwnershipChecker::renderProjection(const Place& place) const {
    std::string result;
    for (const auto& component : place.components) result += component;
    return result;
}

std::string OwnershipChecker::renderPlace(const Place& place) const {
    return place.root + renderProjection(place);
}

bool OwnershipChecker::placesOverlap(const Place& left, const Place& right) const {
    if (left.root != right.root) return false;
    const size_t common = std::min(left.components.size(), right.components.size());
    for (size_t index = 0; index < common; ++index) {
        const auto& a = left.components[index];
        const auto& b = right.components[index];
        const bool wildcard = a == "[*]" || b == "[*]";
        if (a != b && !wildcard) return false;
    }
    return true;
}

bool OwnershipChecker::hasConflictingLoan(const Place& place, bool forMutation) const {
    for (const auto& scope : mLoansInScope) {
        for (const auto& loan : scope) {
            if (!placesOverlap(place, loan.source)) continue;
            if (forMutation || loan.isMutable) return true;
        }
    }
    return false;
}

bool OwnershipChecker::isPlaceAvailable(const Place& place, const std::string& action) {
    auto* var = lookup(place.root);
    if (!var) return true;
    if (var->state == OwnState::Moved) {
        error(action + " after move of '" + renderPlace(place) + "'");
        return false;
    }
    if (var->state == OwnState::Freed) {
        error(action + " after free of '" + renderPlace(place) + "'");
        return false;
    }
    for (const auto& moved : var->movedPlaces) {
        if (placesOverlap(place, moved)) {
            error(action + " of moved place '" + renderPlace(place) + "'");
            return false;
        }
    }
    if (hasConflictingLoan(place, false)) {
        error("Cannot " + action + " '" + renderPlace(place) +
              "' while an overlapping place is mutably borrowed");
        return false;
    }
    return true;
}

bool OwnershipChecker::acquireLoan(const Place& place, bool isMutable) {
    auto* var = lookup(place.root);
    if (!var) {
        error("Borrow of undefined place '" + renderPlace(place) + "'");
        return false;
    }
    if (!isPlaceAvailable(place, "borrow")) return false;
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot borrow device buffer '" + place.root + "' while a launch is in flight");
        return false;
    }
    if (isMutable && var->isReference && !var->isMutableReference) {
        error("Cannot mutably borrow through a shared reference '" + place.root + "'");
        return false;
    }
    if (hasConflictingLoan(place, isMutable)) {
        error("Cannot " + std::string(isMutable ? "mutably " : "") + "borrow '" +
              renderPlace(place) + "' while an overlapping place is borrowed");
        return false;
    }
    if (isMutable) var->mutableBorrow = true;
    else ++var->sharedBorrows;
    mLoansInScope.back().push_back({place, isMutable});
    return true;
}

bool OwnershipChecker::beginInFlightBorrow(const std::string& name, bool isMutable) {
    auto* var = lookup(name);
    if (!var) {
        error("launch borrows undefined device buffer '" + name + "'");
        return false;
    }
    if (!isDeviceBuffer(var->type)) {
        error("launch resource '" + name + "' is not a device buffer");
        return false;
    }
    if (var->state != OwnState::Valid) {
        error("Cannot launch with invalid device buffer '" + name + "'");
        return false;
    }
    if (var->sharedBorrows > 0 || var->mutableBorrow ||
        var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot launch with device buffer '" + name +
              "' while it is borrowed or already in flight");
        return false;
    }
    if (isMutable) ++var->inFlightWrites;
    else ++var->inFlightReads;
    return true;
}

void OwnershipChecker::finishEvent(VarInfo* event) {
    if (!event) return;
    for (const auto& resource : event->eventResources) {
        auto* buffer = lookup(resource.source.root);
        if (!buffer) continue;
        if (resource.isMutable) {
            if (buffer->inFlightWrites > 0) --buffer->inFlightWrites;
        } else if (buffer->inFlightReads > 0) {
            --buffer->inFlightReads;
        }
    }
    event->eventResources.clear();
}

void OwnershipChecker::releaseLoan(const Loan& loan) {
    auto* var = lookup(loan.source.root);
    if (!var) return;
    if (loan.isMutable) var->mutableBorrow = false;
    else if (var->sharedBorrows > 0) --var->sharedBorrows;
}

bool OwnershipChecker::consume(VarInfo* var, const std::string& action) {
    return var && consume(Place{var->name, {}}, action);
}

bool OwnershipChecker::allDirectFieldsMoved(const VarInfo& var) const {
    if (!var.type || var.type->fields.empty()) return false;
    for (const auto& field : var.type->fields) {
        const Place direct{var.name, {"." + field.name}};
        bool covered = false;
        for (const auto& moved : var.movedPlaces) {
            if (moved.components.size() <= direct.components.size() &&
                placesOverlap(direct, moved)) {
                covered = true;
                break;
            }
        }
        if (!covered) return false;
    }
    return true;
}

TypePtr OwnershipChecker::typeOfPlace(const Place& place) const {
    auto* self = const_cast<OwnershipChecker*>(this);
    auto* variable = self->lookup(place.root);
    TypePtr current = variable ? variable->type : nullptr;
    for (const auto& component : place.components) {
        if (!current) return nullptr;
        if (!component.empty() && component.front() == '.') {
            const std::string fieldName = component.substr(1);
            TypePtr next;
            for (const auto& field : current->fields)
                if (field.name == fieldName) { next = field.type; break; }
            current = next;
        } else if (!component.empty() && component.front() == '[') {
            current = current->inner;
        } else if (component == "*") {
            current = current->inner;
        }
    }
    return current;
}

bool OwnershipChecker::consume(const Place& place, const std::string& action) {
    auto* var = lookup(place.root);
    if (!var) {
        error(action + " of undefined place '" + renderPlace(place) + "'");
        return false;
    }
    if (!isPlaceAvailable(place, action)) return false;
    if (!place.components.empty() &&
        defaultUsageForType(typeOfPlace(place)) == luna::ownership::Usage::Copy)
        return true;
    if (!place.components.empty() && var->type &&
        var->type->kind == TypeKind::Record) {
        error("partial move from anonymous record '" + place.root +
              "' is not yet supported; move the whole record");
        return false;
    }
    // A reference binding owns no referent, but its local handle still has a
    // usage contract. Moving that complete handle consumes the binding while
    // the lexical loan remains attached to the source scope. Projections or
    // unqualified borrowed views of heap-shaped values cannot use this path.
    const bool consumesReferenceHandle =
        var->isReference && place.components.empty();
    if (var->relation != luna::ownership::Relation::Owned &&
        !consumesReferenceHandle) {
        error("Cannot " + action + " borrowed place '" + renderPlace(place) +
              "'; declare an owning affine or linear parameter to transfer it");
        return false;
    }
    if (hasConflictingLoan(place, true)) {
        error("Cannot " + action + " '" + renderPlace(place) +
              "' while an overlapping place is borrowed");
        return false;
    }
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot " + action + " device buffer '" + place.root +
              "' while a launch is in flight");
        return false;
    }
    // Moving a Copy value is observationally a copy. Affine and linear values
    // transfer ownership and invalidate precisely the selected place.
    if (var->usage == luna::ownership::Usage::Copy) return true;
    if (place.components.empty()) var->state = OwnState::Moved;
    else {
        var->movedPlaces.push_back(place);
        if (allDirectFieldsMoved(*var)) var->state = OwnState::Moved;
    }
    return true;
}

bool OwnershipChecker::checkWriteTarget(Expr* expr) {
    if (auto* index = dynamic_cast<IndexExpr*>(expr))
        if (!checkExpr(index->index.get())) return false;
    auto place = extractPlace(expr);
    if (!place) return checkExpr(expr);
    auto* var = lookup(place->root);
    if (!var) {
        error("Assignment to undefined place '" + renderPlace(*place) + "'");
        return false;
    }
    if (!isPlaceAvailable(*place, "assignment")) return false;
    if (hasConflictingLoan(*place, true)) {
        error("Cannot assign to '" + place->root +
              "' while it is borrowed (overlapping place '" +
              renderPlace(*place) + "')");
        return false;
    }
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot assign to device buffer '" + place->root + "' while a launch is in flight");
        return false;
    }
    if (luna::ownership::isMoveOnly(var->usage) && place->components.empty()) {
        error("Cannot overwrite " + std::string(luna::ownership::usageName(var->usage)) +
              " variable '" + place->root +
              "' without consuming its current value");
        return false;
    }
    return true;
}

luna::ownership::Usage OwnershipChecker::usageFromTypeAST(const TypeAST* ast) const {
    if (dynamic_cast<const LinearTypeAST*>(ast)) return luna::ownership::Usage::Linear;
    if (dynamic_cast<const AffineTypeAST*>(ast)) return luna::ownership::Usage::Affine;
    return luna::ownership::Usage::Copy;
}

bool OwnershipChecker::isReferenceExpr(Expr* expr) {
    if (dynamic_cast<BorrowExpr*>(expr) || dynamic_cast<AddrOfExpr*>(expr)) return true;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto* var = lookup(id->name);
        return var && var->isReference;
    }
    return false;
}

OwnershipChecker::VarInfo* OwnershipChecker::lookup(const std::string& name) {
    for (auto it = mScopes.rbegin(); it != mScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

OwnershipChecker::VarInfo* OwnershipChecker::lookupCleanupVariable(
    const std::string& encodedPlace) {
    auto [name, ordinal] = decodeCleanupPlace(encodedPlace);
    for (auto scope = mScopes.rbegin(); scope != mScopes.rend(); ++scope) {
        auto found = scope->find(name);
        if (found == scope->end()) continue;
        if (ordinal == 0) return &found->second;
        --ordinal;
    }
    return nullptr;
}

void OwnershipChecker::define(const std::string& name, TypePtr type, bool isHeap,
                              luna::ownership::Usage usage,
                              luna::ownership::Relation relation,
                              bool isReference, bool isMutableReference) {
    VarInfo info;
    info.name = name;
    info.type = type;
    info.isHeapAllocated = relation == luna::ownership::Relation::Owned &&
        (isHeap || typeRequiresCleanup(type));
    // Usage blocks (linear {}, affine {}) are syntactic sugar that only
    // constrain newly declared owning bindings. Borrowed bindings (references)
    // always keep Copy cardinality regardless of the surrounding block.
    info.usage = (isDeviceBuffer(type) || isEvent(type))
        ? luna::ownership::Usage::Linear
        : (relation != luna::ownership::Relation::Owned
            ? luna::ownership::Usage::Copy
            : (usage == luna::ownership::Usage::Copy && info.isHeapAllocated
                ? luna::ownership::Usage::Affine : usage));
    info.relation = relation;
    info.isReference = isReference;
    info.isMutableReference = isMutableReference;
    info.isGpuEvent = isEvent(type);
    mScopes.back()[name] = info;
}

std::vector<std::string> OwnershipChecker::collectFreesAtScopeExit() {
    std::vector<std::string> frees;
    for (auto& [name, info] : mScopes.back()) {
        if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
            info.state == OwnState::Valid)
            frees.push_back(name);
    }
    return frees;
}

std::vector<std::string> OwnershipChecker::collectFreesAtReturn() const {
    std::vector<std::string> frees;
    std::unordered_map<std::string, size_t> shadowOrdinals;
    // Exit order is innermost to outermost, matching lexical destruction.
    for (auto scope = mScopes.rbegin(); scope != mScopes.rend(); ++scope) {
        for (const auto& [name, info] : *scope) {
            const size_t ordinal = shadowOrdinals[name]++;
            if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
                info.state == OwnState::Valid)
                frees.push_back(cleanupPlace(name, ordinal));
        }
    }
    return frees;
}

std::vector<std::string> OwnershipChecker::collectFreesAtFragmentExit() const {
    std::vector<std::string> frees;
    std::unordered_map<std::string, size_t> shadowOrdinals;
    for (size_t index = mScopes.size(); index > mCurrentFragmentScopeBase; --index) {
        for (const auto& [name, info] : mScopes[index - 1]) {
            const size_t ordinal = shadowOrdinals[name]++;
            if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
                info.state == OwnState::Valid)
                frees.push_back(cleanupPlace(name, ordinal));
        }
    }
    return frees;
}

void OwnershipChecker::validateLinearScope() {
    // An unawaited event is the root cause for each buffer it still holds in
    // flight. Report that actionable error once instead of adding a second,
    // derivative "not consumed" diagnostic for the same-scope buffer.
    std::unordered_set<std::string> resourcesHeldByUnawaitedEvents;
    for (const auto& [_, info] : mScopes.back()) {
        if (!info.isGpuEvent || info.state != OwnState::Valid) continue;
        for (const auto& resource : info.eventResources)
            resourcesHeldByUnawaitedEvents.insert(resource.source.root);
    }
    for (const auto& [name, info] : mScopes.back()) {
        if (luna::ownership::mustConsume(info.usage) && info.state == OwnState::Valid) {
            if (info.isGpuEvent)
                error("launch event '" + name + "' was not awaited before leaving its scope");
            else if (isDeviceBuffer(info.type) && resourcesHeldByUnawaitedEvents.count(name))
                continue;
            else
                error("Linear variable '" + name + "' was not consumed before leaving its scope");
        }
    }
}

void OwnershipChecker::validateLinearReturnPath() {
    // Returning exits every active lexical scope, not only the innermost
    // block that contains the `return`.  Checking the complete stack here
    // makes an early return as strict as ordinary fall-through scope exit.
    std::unordered_set<std::string> resourcesHeldByUnawaitedEvents;
    for (const auto& scope : mScopes) {
        for (const auto& [_, info] : scope) {
            if (!info.isGpuEvent || info.state != OwnState::Valid) continue;
            for (const auto& resource : info.eventResources)
                resourcesHeldByUnawaitedEvents.insert(resource.source.root);
        }
    }
    for (const auto& scope : mScopes) {
        for (const auto& [name, info] : scope) {
            if (!luna::ownership::mustConsume(info.usage) ||
                info.state != OwnState::Valid) continue;
            if (info.isGpuEvent)
                error("launch event '" + name + "' was not awaited before returning");
            else if (isDeviceBuffer(info.type) && resourcesHeldByUnawaitedEvents.count(name))
                continue;
            else
                error("Linear variable '" + name + "' was not consumed before returning");
        }
    }
}

bool OwnershipChecker::isDeviceBuffer(const TypePtr& type) const {
    return type && type->kind == TypeKind::DeviceBuffer;
}

bool OwnershipChecker::isEvent(const TypePtr& type) const {
    return type && type->kind == TypeKind::Event;
}

void OwnershipChecker::error(const std::string& msg, int line, int col) {
    if (line <= 0) line = mDiagnosticLine;
    if (col <= 0) col = mDiagnosticCol;
    std::string hint;
    if (msg.find("after move") != std::string::npos)
        hint = "use the value before `move`, borrow it instead, or create a replacement value";
    else if (msg.find("in flight") != std::string::npos || msg.find("was not awaited") != std::string::npos)
        hint = "bind the launch result and call `await event` before reusing, moving, or freeing the device buffer";
    else if (msg.find("borrow") != std::string::npos)
        hint = "a value may have many shared borrows or one mutable borrow, but not both";
    else if (msg.find("Linear variable") != std::string::npos)
        hint = "consume it with `move`, pass it to an owning operation, or `free` it when it is heap-allocated";
    else if (msg.find("cannot be resumed more than once") != std::string::npos)
        hint = "make the fragment single-shot, or ensure the continuation does not consume, free, or mutate captured ownership state";
    else if (msg.find("free") != std::string::npos)
        hint = "only heap-allocated values may be explicitly freed";
    mErrors.push_back(diagnostic::format(
        "ownership", msg, mDiagnosticFile, line, col, hint,
        diagnostic::sourceLineFromFile(mDiagnosticFile, line)));
}

void OwnershipChecker::setDiagnosticLocation(const ASTNode* node) {
    if (!node) return;
    if (!node->sourcePath.empty()) mDiagnosticFile = node->sourcePath;
    if (node->line > 0) mDiagnosticLine = node->line;
    if (node->col > 0) mDiagnosticCol = node->col;
}
