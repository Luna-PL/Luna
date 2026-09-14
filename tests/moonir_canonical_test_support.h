#pragma once

#include <llvm/Support/Compiler.h>

#include "moonir/ControlFlowBuilder.h"
#include "moonir/Verifier.h"

#include <iostream>
#include <memory>
#include <string>

namespace canonical_test {

struct ControlFlowTestContext {
    moon::Module& module;
    moon::Verifier& verifier;
    moon::ControlFlowBuilder& builder;
    const TypePtr& rangeIterator;
    const TypePtr& ownedProduct;
    moon::TypeRef shortId;
    moon::TypeRef rangeId;
    moon::TypeRef sharedId;
    moon::TypeRef sliceId;
    moon::TypeRef sharedSliceId;
    moon::TypeRef mutableSliceId;
    moon::TypeRef productId;
    moon::TypeRef ownedProductId;
    moon::TypeRef inlineProductId;
    moon::TypeRef i32Id;
    moon::TypeRef usizeId;
    moon::TypeRef stringId;
    moon::TypeRef guardedArrayId;
    moon::TypeRef boolId;
    moon::TypeRef unitId;
    moon::TypeRef resultI32BoolId;
    moon::TypeRef lambdaTypeId;
    moon::TypeRef closureTypeId;
    moon::TypeRef predicateTypeId;
    moon::TypeRef reducerTypeId;
    moon::TypeRef affineReducerTypeId;
    moon::TypeRef linearReducerTypeId;
    moon::TypeRef affineValueProducerTypeId;
    moon::TypeRef actionTypeId;
    moon::TypeRef unitConsumerTypeId;
    moon::TypeRef unitOrderedConsumerTypeId;
    moon::TypeRef moveMapTypeId;
    moon::TypeRef affinePredicateTypeId;
    moon::TypeRef affineIdentityTypeId;
    moon::TypeRef moveMapIteratorId;
    moon::TypeRef choiceId;
};

struct SealingTestContext {
    moon::ControlFlowBuilder& builder;
    moon::Verifier& cfgVerifier;
    moon::Verifier& verifier;
    moon::Module& module;
    moon::Module& reverseModule;
    moon::TypeRef shortIteratorType;
    moon::TypeRef productType;
};

inline std::unique_ptr<moon::LetStmt> makeMaterializedRangeBinding(
    const ControlFlowTestContext& context,
    const std::string& name,
    bool withTake = true) {
    auto binding = std::make_unique<moon::LetStmt>();
    binding->name = name;
    binding->type = context.rangeId;
    binding->usage = luna::ownership::Usage::Affine;
    binding->materializesIteratorRecipe = true;
    auto source = std::make_unique<moon::CallExpr>();
    source->iteratorOp = IteratorOp::Range;
    source->iteratorInputType = context.i32Id;
    source->iteratorOutputType = context.i32Id;
    source->type = context.rangeId;
    auto sourceCallee = std::make_unique<moon::IdentifierExpr>();
    sourceCallee->name = "range";
    source->callee = std::move(sourceCallee);
    auto start = std::make_unique<moon::IntLiteralExpr>();
    start->value = 1;
    start->type = context.i32Id;
    auto end = std::make_unique<moon::IntLiteralExpr>();
    end->value = 5;
    end->type = context.i32Id;
    source->args.push_back(std::move(start));
    source->args.push_back(std::move(end));
    if (!withTake) {
        binding->initializer = std::move(source);
        return binding;
    }
    auto take = std::make_unique<moon::CallExpr>();
    take->iteratorOp = IteratorOp::Take;
    take->iteratorInputType = context.i32Id;
    take->iteratorOutputType = context.i32Id;
    take->type = context.rangeId;
    auto takeMember = std::make_unique<moon::FieldAccessExpr>();
    takeMember->field = "take";
    takeMember->object = std::move(source);
    take->callee = std::move(takeMember);
    auto count = std::make_unique<moon::IntLiteralExpr>();
    count->value = 2;
    count->type = context.i32Id;
    take->args.push_back(std::move(count));
    binding->initializer = std::move(take);
    return binding;
}

inline std::unique_ptr<moon::CallExpr> makeIteratorTerminal(
    const ControlFlowTestContext& context,
    const std::string& recipeName,
    IteratorOp op,
    moon::TypeRef resultType,
    moon::TypeRef outputType) {
    auto call = std::make_unique<moon::CallExpr>();
    call->iteratorOp = op;
    call->iteratorInputType = context.i32Id;
    call->iteratorOutputType = outputType;
    call->type = resultType;
    auto member = std::make_unique<moon::FieldAccessExpr>();
    auto recipe = std::make_unique<moon::IdentifierExpr>();
    recipe->name = recipeName;
    recipe->type = context.rangeId;
    member->object = std::move(recipe);
    member->field = op == IteratorOp::Fold
        ? "fold"
        : (op == IteratorOp::ForEach
               ? "for_each"
               : (op == IteratorOp::Collect ? "collect" : "count"));
    call->callee = std::move(member);
    return call;
}

inline std::unique_ptr<moon::CallExpr> makeDirectRangeTerminal(
    const ControlFlowTestContext& context,
    IteratorOp op,
    moon::TypeRef resultType,
    moon::TypeRef outputType,
    bool withTake) {
    auto range = std::make_unique<moon::CallExpr>();
    range->iteratorOp = IteratorOp::Range;
    range->iteratorInputType = context.i32Id;
    range->iteratorOutputType = context.i32Id;
    range->type = context.rangeId;
    auto rangeCallee = std::make_unique<moon::IdentifierExpr>();
    rangeCallee->name = "range";
    range->callee = std::move(rangeCallee);
    auto start = std::make_unique<moon::IntLiteralExpr>();
    start->value = 1;
    start->type = context.i32Id;
    auto end = std::make_unique<moon::IntLiteralExpr>();
    end->value = 5;
    end->type = context.i32Id;
    range->args.push_back(std::move(start));
    range->args.push_back(std::move(end));

    std::unique_ptr<moon::Expr> recipe = std::move(range);
    if (withTake) {
        auto take = std::make_unique<moon::CallExpr>();
        take->iteratorOp = IteratorOp::Take;
        take->iteratorInputType = context.i32Id;
        take->iteratorOutputType = context.i32Id;
        take->type = context.rangeId;
        auto takeMember = std::make_unique<moon::FieldAccessExpr>();
        takeMember->field = "take";
        takeMember->object = std::move(recipe);
        take->callee = std::move(takeMember);
        auto count = std::make_unique<moon::IntLiteralExpr>();
        count->value = 3;
        count->type = context.i32Id;
        take->args.push_back(std::move(count));
        recipe = std::move(take);
    }

    auto terminal = std::make_unique<moon::CallExpr>();
    terminal->iteratorOp = op;
    terminal->iteratorInputType = context.i32Id;
    terminal->iteratorOutputType = outputType;
    terminal->type = resultType;
    auto member = std::make_unique<moon::FieldAccessExpr>();
    member->object = std::move(recipe);
    member->field = op == IteratorOp::Fold
        ? "fold"
        : (op == IteratorOp::ForEach
               ? "for_each"
               : (op == IteratorOp::Collect ? "collect" : "count"));
    terminal->callee = std::move(member);
    return terminal;
}

inline int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

inline void trace(const char* phase) {
    std::cerr << "[moonir-canonical] " << phase << '\n';
}

using GenerationEntry = int (*)();
using UnaryGenerationEntry = int (*)(int);

#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
inline int invokeGenerationEntry(const void* address) {
    return reinterpret_cast<GenerationEntry>(
        const_cast<void*>(address))();
}

#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
inline int invokeUnaryGenerationEntry(const void* address, int value) {
    return reinterpret_cast<UnaryGenerationEntry>(
        const_cast<void*>(address))(value);
}

int runRegisteredTests();
int runControlFlowTests(ControlFlowTestContext& context);
int runIteratorTests(ControlFlowTestContext& context);
int runIteratorRecipeTests(ControlFlowTestContext& context);
int runIteratorTerminalTests(ControlFlowTestContext& context);
int runIteratorOrderingTests(ControlFlowTestContext& context);
int runIteratorCleanupTests(ControlFlowTestContext& context);
int runReferenceAndClosureTests(ControlFlowTestContext& context);
int runFunctionSealingTests(SealingTestContext& context);
int runCompositionSealingTests(SealingTestContext& context);
int runLoweredCompositionTests(SealingTestContext& context);
int runSymbolSealingTests(SealingTestContext& context);
int runIteratorSealingTests(SealingTestContext& context);
int runSealingTests(
    moon::ControlFlowBuilder& builder,
    moon::Verifier& cfgVerifier,
    moon::Module& module,
    moon::Module& reverseModule,
    const moon::TypeRef& shortIteratorType,
    const moon::TypeRef& productType);

} // namespace canonical_test
