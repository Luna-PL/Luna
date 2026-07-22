#pragma once

#include "../core/TypeSystem.h"

#include <string>
#include <unordered_map>

// Sema-only inference state. Inference variables are never valid MoonIR
// types; the Moon verifier rejects them if frontend materialization misses
// one.
class ConstraintSolver {
public:
    TypePtr fresh();
    TypePtr resolve(const TypePtr& type);
    bool unify(const TypePtr& lhs, const TypePtr& rhs,
               std::string* reason = nullptr);
    void requireNumeric(const TypePtr& type);
    void requireBool(const TypePtr& type);
    void defaultUnconstrainedNumeric();
    bool hasUnresolved(const TypePtr& type);

private:
    bool unifyInternal(const TypePtr& lhs, const TypePtr& rhs,
                       std::string* reason);
    bool contains(const TypePtr& type, int id);
    void collectUnresolvedNumeric(const TypePtr& type);

    int mNextId = 0;
    std::unordered_map<int, TypePtr> mBindings;
    std::unordered_map<int, bool> mNumericConstraints;
    std::unordered_map<int, bool> mBoolConstraints;
};
