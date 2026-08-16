#include "moonir/MoonIR.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "sema/SymbolTable.h"
#include "tooling/AnalysisSnapshot.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(moon::MetadataField::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::DeclarationRecord::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Param::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Expr::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::LetStmt::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::intrinsicType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::calleeRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::ForStmt::protocolNext), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::LaunchExpr::kernelRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::TryExpr::errorConversion), moon::DeclarationRef>);
static_assert(!std::is_same_v<moon::BlockId, moon::ScopeId>);
static_assert(!std::is_same_v<moon::LocalId, moon::CleanupId>);
static_assert(std::is_same_v<decltype(moon::FunctionDecl::returnType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::StructDecl::type), moon::TypeRef>);

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    moon::ControlFlowGraph cfg;
    cfg.entry = moon::BlockId{0};
    cfg.rootRegion = moon::RegionId{0};
    cfg.rootScope = moon::ScopeId{0};
    cfg.blocks.emplace_back();
    cfg.blocks.back().id = cfg.entry;
    cfg.blocks.back().region = cfg.rootRegion;
    cfg.blocks.back().scope = cfg.rootScope;
    cfg.blocks.back().terminator.kind = moon::TerminatorKind::Return;
    cfg.regions.push_back({cfg.rootRegion, {}, moon::RegionKind::Function,
                           cfg.rootScope, cfg.entry, {}, {cfg.entry}, {}, {}, {}});
    cfg.scopes.push_back({cfg.rootScope, {}, cfg.rootRegion, {}, {}, {}});
    if (!cfg.findBlock(cfg.entry) || !cfg.findRegion(cfg.rootRegion) ||
        !cfg.findScope(cfg.rootScope) ||
        cfg.findBlock(moon::BlockId{1}))
        return fail("canonical CFG table references are not index-stable");

    auto shortArray = Type::makeArray(TyI32, 2);
    auto longArray = Type::makeArray(TyI32, 5);
    auto shortIterator = Type::makeIterator(
        TyI32, IteratorMode::Consuming, shortArray);
    auto longIterator = Type::makeIterator(
        TyI32, IteratorMode::Consuming, longArray);
    auto rangeIterator = Type::makeIterator(
        TyI32, IteratorMode::Range);
    auto sharedIterator = Type::makeIterator(
        Type::makeReference(TyI32), IteratorMode::Shared, shortArray);
    auto slice = Type::makeSlice(TyI32);
    auto sharedSliceIterator = Type::makeIterator(
        Type::makeReference(TyI32), IteratorMode::Shared, slice);
    auto mutableSliceIterator = Type::makeIterator(
        Type::makeReference(TyI32, true), IteratorMode::Mutable, slice);
    auto guardedArray = Type::makeArray(TyString, 2);

    moon::Module module;
    module.name = "canonical.test";
    const auto shortId = module.registerType(shortIterator);
    const auto longId = module.registerType(longIterator);
    const auto rangeId = module.registerType(rangeIterator);
    const auto sharedId = module.registerType(sharedIterator);
    const auto sliceId = module.registerType(slice);
    const auto sharedSliceId = module.registerType(sharedSliceIterator);
    const auto mutableSliceId = module.registerType(mutableSliceIterator);
    if (shortId == longId)
        return fail("backend-significant iterator sources collapsed to one TypeId");

    auto product = Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot");
    const auto productId = module.registerType(product);
    auto ownedProduct = Type::makeStruct(
        "OwnedSnapshot", {{"owned", TyString}, {"value", TyI32}},
        "canonical.test::OwnedSnapshot");
    const auto ownedProductId = module.registerType(ownedProduct);
    auto inlineProduct = Type::makeRecord({{"value", TyI32}});
    const auto inlineProductId = module.registerType(inlineProduct);
    const auto i32Id = module.registerType(TyI32);
    const auto usizeId = module.registerType(TyUSize);
    const auto stringId = module.registerType(TyString);
    const auto guardedArrayId = module.registerType(guardedArray);
    const auto boolId = module.registerType(TyBool);
    const auto unitId = module.registerType(TyUnit);
    auto resultI32Bool = Type::makeResult(TyI32, TyBool);
    const auto resultI32BoolId = module.registerType(resultI32Bool);
    auto lambdaType = Type::makeFunction(
        {TyI32}, TyI32,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto lambdaTypeId = module.registerType(lambdaType);
    auto closureType = Type::makeClosure(
        {TyI32}, TyI32,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy},
        {{"offset", TyI32}});
    const auto closureTypeId = module.registerType(closureType);
    auto predicateType = Type::makeFunction(
        {TyI32}, TyBool,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto predicateTypeId = module.registerType(predicateType);
    auto reducerType = Type::makeFunction(
        {TyI32, TyI32}, TyI32,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy},
         {luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto reducerTypeId = module.registerType(reducerType);
    auto affineReducerType = Type::makeFunction(
        {TyString, TyI32}, TyString,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine},
         {luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    const auto affineReducerTypeId = module.registerType(
        affineReducerType);
    auto linearReducerType = Type::makeFunction(
        {TyI32, TyI32}, TyI32,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Linear},
         {luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto linearReducerTypeId = module.registerType(
        linearReducerType);
    auto affineValueProducerType = Type::makeFunction(
        {}, TyI32, {},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    const auto affineValueProducerTypeId = module.registerType(
        affineValueProducerType);
    auto actionType = Type::makeFunction(
        {TyI32}, TyUnit,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto actionTypeId = module.registerType(actionType);
    auto unitConsumerType = Type::makeFunction(
        {TyUnit}, TyUnit,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto unitConsumerTypeId = module.registerType(unitConsumerType);
    auto unitOrderedConsumerType = Type::makeFunction(
        {TyUnit, TyI32}, TyUnit,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy},
         {luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto unitOrderedConsumerTypeId = module.registerType(
        unitOrderedConsumerType);
    auto moveMapType = Type::makeFunction(
        {TyI32}, TyString,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    const auto moveMapTypeId = module.registerType(moveMapType);
    auto affinePredicateType = Type::makeFunction(
        {TyString}, TyBool,
        {{luna::ownership::Relation::SharedBorrow,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto affinePredicateTypeId = module.registerType(
        affinePredicateType);
    auto affineIdentityType = Type::makeFunction(
        {TyString}, TyString,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    const auto affineIdentityTypeId = module.registerType(
        affineIdentityType);
    auto moveMapIterator = Type::makeIterator(
        TyString, IteratorMode::Range, rangeIterator);
    const auto moveMapIteratorId = module.registerType(moveMapIterator);
    auto choiceType = Type::makeEnum(
        "Choice", {{"None", {}}, {"Some", {TyI32}}},
        "canonical.test::Choice");
    const auto choiceId = module.registerType(choiceType);
    auto forward = Type::makeStruct(
        "Forward", {}, "canonical.test::Forward");
    auto completedForward = Type::makeStruct(
        "Forward", {{"value", TyI32}}, "canonical.test::Forward");
    const auto forwardId = module.registerType(forward);
    if (module.registerType(completedForward) != forwardId)
        return fail("completed nominal declaration changed its stable TypeId");
    module.sealTypeTable();
    cfg.sealed = true;
    moon::Verifier cfgVerifier;
    if (!cfgVerifier.verify(cfg, module))
        return fail("canonical CFG foundation failed independent verification");
    cfg.blocks.front().id = moon::BlockId{1};
    if (cfgVerifier.verify(cfg, module))
        return fail("CFG verifier accepted a forged canonical block index");
    cfg.blocks.front().id = cfg.entry;

    moon::ControlFlowGraph cleanupCfg;
    cleanupCfg.sealed = true;
    cleanupCfg.entry = moon::BlockId{0};
    cleanupCfg.rootRegion = moon::RegionId{0};
    cleanupCfg.rootScope = moon::ScopeId{0};
    cleanupCfg.blocks.resize(3);
    for (uint32_t index = 0; index < cleanupCfg.blocks.size(); ++index)
        cleanupCfg.blocks[index].id = moon::BlockId{index};
    cleanupCfg.blocks[0].region = moon::RegionId{0};
    cleanupCfg.blocks[0].scope = moon::ScopeId{0};
    cleanupCfg.blocks[0].terminator.kind = moon::TerminatorKind::Jump;
    cleanupCfg.blocks[0].terminator.primary.target = moon::BlockId{1};
    cleanupCfg.blocks[1].region = moon::RegionId{1};
    cleanupCfg.blocks[1].scope = moon::ScopeId{1};
    auto cleanupBinding = std::make_unique<moon::LetStmt>();
    cleanupBinding->name = "owned";
    cleanupBinding->local = moon::LocalId{0};
    cleanupBinding->usage = luna::ownership::Usage::Affine;
    cleanupBinding->relation = luna::ownership::Relation::Owned;
    cleanupBinding->type = stringId;
    auto cleanupValue = std::make_unique<moon::StringLiteralExpr>();
    cleanupValue->value = "owned";
    cleanupValue->type = stringId;
    cleanupBinding->initializer = std::move(cleanupValue);
    cleanupCfg.blocks[1].operations.push_back(std::move(cleanupBinding));
    cleanupCfg.blocks[1].terminator.kind = moon::TerminatorKind::Jump;
    cleanupCfg.blocks[1].terminator.primary.target = moon::BlockId{2};
    cleanupCfg.blocks[1].terminator.primary.cleanups = {moon::CleanupId{0}};
    cleanupCfg.blocks[2].region = moon::RegionId{0};
    cleanupCfg.blocks[2].scope = moon::ScopeId{0};
    cleanupCfg.blocks[2].terminator.kind = moon::TerminatorKind::Return;
    cleanupCfg.regions.push_back({moon::RegionId{0}, {},
        moon::RegionKind::Function, moon::ScopeId{0}, moon::BlockId{0}, {},
        {moon::BlockId{0}, moon::BlockId{2}}, {}, {}, {}});
    cleanupCfg.regions.push_back({moon::RegionId{1}, moon::RegionId{0},
        moon::RegionKind::Lexical, moon::ScopeId{1}, moon::BlockId{1},
        moon::BlockId{2}, {moon::BlockId{1}}, {}, {}, {}});
    cleanupCfg.scopes.push_back({moon::ScopeId{0}, {}, moon::RegionId{0},
                                 {}, {}, {}});
    cleanupCfg.scopes.push_back({moon::ScopeId{1}, moon::ScopeId{0},
        moon::RegionId{1}, {moon::LocalId{0}}, {moon::CleanupId{0}}, {}});
    cleanupCfg.locals.push_back({moon::LocalId{0}, moon::ScopeId{1},
        moon::LocalKind::Binding, "owned", stringId,
        luna::ownership::Usage::Affine,
        luna::ownership::Relation::Owned});
    cleanupCfg.cleanups.push_back({moon::CleanupId{0}, moon::ScopeId{1},
        {moon::LocalId{0}, {}}, stringId,
        moon::CleanupKind::Value,
        luna::ownership::CleanupAction::Deallocate, {}});
    if (!cfgVerifier.verify(cleanupCfg, module))
        return fail("CFG verifier rejected a canonical scope-exit cleanup edge");
    cleanupCfg.blocks[1].terminator.primary.cleanups.clear();
    if (cfgVerifier.verify(cleanupCfg, module))
        return fail("CFG verifier accepted an omitted scope-exit cleanup");

    moon::ControlFlowGraph guardedCleanupCfg;
    guardedCleanupCfg.sealed = true;
    guardedCleanupCfg.entry = moon::BlockId{0};
    guardedCleanupCfg.rootRegion = moon::RegionId{0};
    guardedCleanupCfg.rootScope = moon::ScopeId{0};
    guardedCleanupCfg.blocks.emplace_back();
    guardedCleanupCfg.blocks.back().id = moon::BlockId{0};
    guardedCleanupCfg.blocks.back().region = moon::RegionId{0};
    guardedCleanupCfg.blocks.back().scope = moon::ScopeId{0};
    guardedCleanupCfg.blocks.back().terminator.kind =
        moon::TerminatorKind::Return;
    guardedCleanupCfg.blocks.back().terminator.exitCleanups = {
        moon::CleanupId{1}, moon::CleanupId{0}};
    guardedCleanupCfg.regions.push_back({
        moon::RegionId{0}, {}, moon::RegionKind::Function,
        moon::ScopeId{0}, moon::BlockId{0}, {}, {moon::BlockId{0}},
        {}, {}, {}});
    guardedCleanupCfg.scopes.push_back({
        moon::ScopeId{0}, {}, moon::RegionId{0},
        {moon::LocalId{0}, moon::LocalId{1}},
        {moon::CleanupId{0}, moon::CleanupId{1}}, {}});
    guardedCleanupCfg.locals.push_back({
        moon::LocalId{0}, moon::ScopeId{0}, moon::LocalKind::Binding,
        "source", guardedArrayId, luna::ownership::Usage::Affine,
        luna::ownership::Relation::Owned});
    guardedCleanupCfg.locals.push_back({
        moon::LocalId{1}, moon::ScopeId{0}, moon::LocalKind::Synthetic,
        "$source.next-unread", i32Id, luna::ownership::Usage::Copy,
        luna::ownership::Relation::Owned});
    auto guardedSource = std::make_unique<moon::LetStmt>();
    guardedSource->name = "source";
    guardedSource->local = moon::LocalId{0};
    guardedSource->usage = luna::ownership::Usage::Affine;
    guardedSource->relation = luna::ownership::Relation::Owned;
    guardedSource->type = guardedArrayId;
    auto guardedArrayValue = std::make_unique<moon::ArrayLiteralExpr>();
    guardedArrayValue->type = guardedArrayId;
    guardedArrayValue->elementType = stringId;
    for (const char* value : {"first", "second"}) {
        auto element = std::make_unique<moon::StringLiteralExpr>();
        element->value = value;
        element->type = stringId;
        guardedArrayValue->elements.push_back(std::move(element));
    }
    guardedSource->initializer = std::move(guardedArrayValue);
    guardedCleanupCfg.blocks.back().operations.push_back(
        std::move(guardedSource));
    auto guardedCursor = std::make_unique<moon::LetStmt>();
    guardedCursor->name = "$source.next-unread";
    guardedCursor->local = moon::LocalId{1};
    guardedCursor->usage = luna::ownership::Usage::Copy;
    guardedCursor->relation = luna::ownership::Relation::Owned;
    guardedCursor->type = i32Id;
    auto guardedCursorValue = std::make_unique<moon::IntLiteralExpr>();
    guardedCursorValue->value = 0;
    guardedCursorValue->type = i32Id;
    guardedCursor->initializer = std::move(guardedCursorValue);
    guardedCleanupCfg.blocks.back().operations.push_back(
        std::move(guardedCursor));
    for (uint64_t element = 0; element < 2; ++element) {
        moon::CleanupRecord cleanup;
        cleanup.id = moon::CleanupId{static_cast<uint32_t>(element)};
        cleanup.scope = moon::ScopeId{0};
        cleanup.place.root = moon::LocalId{0};
        cleanup.place.projections.push_back({
            moon::ProjectionKind::ConstantIndex, element, {}});
        cleanup.type = stringId;
        cleanup.kind = moon::CleanupKind::Value;
        cleanup.action = luna::ownership::CleanupAction::Deallocate;
        cleanup.guard = moon::CleanupGuard{moon::LocalId{1}, element};
        guardedCleanupCfg.cleanups.push_back(std::move(cleanup));
    }
    if (!cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("CFG verifier rejected canonical guarded array cleanup state");
    guardedCleanupCfg.cleanups[1].guard.reset();
    if (cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("CFG verifier accepted mixed guarded and unguarded array cleanup");
    guardedCleanupCfg.cleanups[1].guard =
        moon::CleanupGuard{moon::LocalId{1}, 1};
    guardedCleanupCfg.cleanups[1].guard->elementIndex = 0;
    guardedCleanupCfg.cleanups[1].place.projections.front().index = 0;
    if (cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("CFG verifier accepted duplicate guarded array state");
    guardedCleanupCfg.cleanups[1].guard->elementIndex = 1;
    guardedCleanupCfg.cleanups[1].place.projections.front().index = 1;
    guardedCleanupCfg.cleanups[1].guard->nextUnread = moon::LocalId{0};
    if (cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("CFG verifier accepted a non-cursor cleanup guard");
    guardedCleanupCfg.cleanups[1].guard->nextUnread = moon::LocalId{1};
    if (!cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("restored guarded array cleanup state no longer verifies");
    auto* guardedCursorDefinition = dynamic_cast<moon::LetStmt*>(
        guardedCleanupCfg.blocks.front().operations[1].get());
    auto* guardedCursorInitializer = guardedCursorDefinition
        ? dynamic_cast<moon::IntLiteralExpr*>(
              guardedCursorDefinition->initializer.get()) : nullptr;
    if (!guardedCursorInitializer)
        return fail("guarded array fixture lost its cursor initializer");
    guardedCursorInitializer->value = 1;
    if (cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("CFG verifier accepted a nonzero guarded array cursor");
    guardedCursorInitializer->value = 0;
    if (!cfgVerifier.verify(guardedCleanupCfg, module))
        return fail("restored guarded cursor initializer no longer verifies");

    auto structured = std::make_unique<moon::BlockStmt>();
    auto binding = std::make_unique<moon::LetStmt>();
    binding->name = "owned";
    binding->usage = luna::ownership::Usage::Affine;
    binding->type = stringId;
    auto text = std::make_unique<moon::StringLiteralExpr>();
    text->value = "cfg";
    text->type = stringId;
    binding->initializer = std::move(text);
    structured->stmts.push_back(std::move(binding));
    auto use = std::make_unique<moon::ExprStmt>();
    auto usedIdentifier = std::make_unique<moon::IdentifierExpr>();
    usedIdentifier->name = "owned";
    usedIdentifier->type = stringId;
    use->expr = std::move(usedIdentifier);
    structured->stmts.push_back(std::move(use));
    auto branch = std::make_unique<moon::IfStmt>();
    auto condition = std::make_unique<moon::BoolLiteralExpr>();
    condition->value = true;
    condition->type = boolId;
    branch->cond = std::move(condition);
    branch->thenBlock = std::make_unique<moon::BlockStmt>();
    branch->elseBranch = std::make_unique<moon::BlockStmt>();
    structured->stmts.push_back(std::move(branch));
    auto loop = std::make_unique<moon::WhileStmt>();
    auto loopCondition = std::make_unique<moon::BoolLiteralExpr>();
    loopCondition->value = false;
    loopCondition->type = boolId;
    loop->cond = std::move(loopCondition);
    loop->body = std::make_unique<moon::BlockStmt>();
    structured->stmts.push_back(std::move(loop));
    auto match = std::make_unique<moon::MatchStmt>();
    match->matchedType = choiceId;
    auto selected = std::make_unique<moon::VariantConstructExpr>();
    selected->typeName = "Choice";
    selected->variantName = "Some";
    selected->constructedType = choiceId;
    selected->type = choiceId;
    auto selectedValue = std::make_unique<moon::IntLiteralExpr>();
    selectedValue->value = 7;
    selectedValue->type = i32Id;
    selected->args.push_back(std::move(selectedValue));
    match->scrutinee = std::move(selected);
    moon::MatchArm noneArm;
    noneArm.variantName = "None";
    noneArm.variantIndex = 0;
    noneArm.body = std::make_unique<moon::BlockStmt>();
    match->arms.push_back(std::move(noneArm));
    moon::MatchArm someArm;
    someArm.variantName = "Some";
    someArm.variantIndex = 1;
    someArm.bindings = {"value"};
    someArm.bindingTypes = {i32Id};
    someArm.bindingUsages = {luna::ownership::Usage::Copy};
    someArm.body = std::make_unique<moon::BlockStmt>();
    auto patternUse = std::make_unique<moon::ExprStmt>();
    auto patternIdentifier = std::make_unique<moon::IdentifierExpr>();
    patternIdentifier->name = "value";
    patternIdentifier->type = i32Id;
    patternUse->expr = std::move(patternIdentifier);
    someArm.body->stmts.push_back(std::move(patternUse));
    match->arms.push_back(std::move(someArm));
    structured->stmts.push_back(std::move(match));
    auto implicitCleanup = std::make_unique<moon::FreeStmt>();
    implicitCleanup->isImplicit = true;
    implicitCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto cleanedIdentifier = std::make_unique<moon::IdentifierExpr>();
    cleanedIdentifier->name = "owned";
    cleanedIdentifier->type = stringId;
    implicitCleanup->operand = std::move(cleanedIdentifier);
    structured->stmts.push_back(std::move(implicitCleanup));

    moon::ControlFlowBuilder cfgBuilder;
    auto loweredCfg = cfgBuilder.build(
        std::move(structured), {}, moon::RegionKind::Function, module);
    if (!loweredCfg || !cfgVerifier.verify(*loweredCfg, module))
        return fail("structured if/fallthrough cleanup did not become canonical CFG");
    if (loweredCfg->blocks.size() != 11 || loweredCfg->cleanups.size() != 1 ||
        loweredCfg->blocks.front().operations.size() != 2 ||
        loweredCfg->blocks.back().terminator.kind !=
            moon::TerminatorKind::Return)
        return fail("canonical CFG builder retained structured control or cleanup operations");
    auto* loweredUse = dynamic_cast<moon::ExprStmt*>(
        loweredCfg->blocks.front().operations[1].get());
    auto* loweredUseId = loweredUse
        ? dynamic_cast<moon::IdentifierExpr*>(loweredUse->expr.get()) : nullptr;
    if (!loweredUseId || loweredUseId->local != moon::LocalId{0})
        return fail("canonical CFG builder did not bind a stable LocalId");
    loweredUseId->local = moon::LocalId{99};
    if (cfgVerifier.verify(*loweredCfg, module))
        return fail("CFG verifier accepted a forged LocalId use");
    loweredUseId->local = {};
    if (cfgVerifier.verify(*loweredCfg, module))
        return fail("CFG verifier accepted an unresolved local identifier");
    loweredUseId->local = moon::LocalId{0};

    moon::Param guardedArrayParameter;
    guardedArrayParameter.name = "values";
    guardedArrayParameter.usage = luna::ownership::Usage::Affine;
    guardedArrayParameter.relation = luna::ownership::Relation::Owned;
    guardedArrayParameter.type = guardedArrayId;
    auto guardedLoopBody = std::make_unique<moon::BlockStmt>();
    auto guardedLoop = std::make_unique<moon::ForStmt>();
    guardedLoop->varName = "value";
    guardedLoop->elementType = stringId;
    guardedLoop->bindingUsage = luna::ownership::Usage::Affine;
    guardedLoop->recipeStateName = "$for.recipe.values";
    guardedLoop->recipeSourceType = guardedArrayId;
    auto guardedSourceMove = std::make_unique<moon::MoveExpr>();
    guardedSourceMove->type = guardedArrayId;
    auto guardedSourceIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    guardedSourceIdentifier->name = "values";
    guardedSourceIdentifier->type = guardedArrayId;
    guardedSourceMove->operand = std::move(guardedSourceIdentifier);
    guardedLoop->iterable = std::move(guardedSourceMove);
    guardedLoop->body = std::make_unique<moon::BlockStmt>();
    auto guardedEarlyReturn = std::make_unique<moon::IfStmt>();
    auto guardedEarlyReturnCondition =
        std::make_unique<moon::BoolLiteralExpr>();
    guardedEarlyReturnCondition->value = false;
    guardedEarlyReturnCondition->type = boolId;
    guardedEarlyReturn->cond = std::move(guardedEarlyReturnCondition);
    guardedEarlyReturn->thenBlock = std::make_unique<moon::BlockStmt>();
    auto guardedLoopReturn = std::make_unique<moon::ReturnStmt>();
    guardedLoopReturn->cleanups.push_back({
        "value", luna::ownership::CleanupAction::Deallocate, stringId});
    guardedEarlyReturn->thenBlock->stmts.push_back(
        std::move(guardedLoopReturn));
    guardedLoop->body->stmts.push_back(
        std::move(guardedEarlyReturn));
    auto guardedItemCleanup = std::make_unique<moon::FreeStmt>();
    guardedItemCleanup->isImplicit = true;
    guardedItemCleanup->action =
        luna::ownership::CleanupAction::Deallocate;
    auto guardedItemIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    guardedItemIdentifier->name = "value";
    guardedItemIdentifier->type = stringId;
    guardedItemCleanup->operand = std::move(guardedItemIdentifier);
    guardedLoop->body->stmts.push_back(std::move(guardedItemCleanup));
    guardedLoopBody->stmts.push_back(std::move(guardedLoop));
    auto guardedLoopCfg = cfgBuilder.build(
        std::move(guardedLoopBody), {guardedArrayParameter},
        moon::RegionKind::Function, module);
    moon::MoveExpr* guardedElementTransfer = nullptr;
    moon::LocalId guardedNextUnread;
    std::vector<moon::CleanupId> guardedTailCleanups;
    std::vector<moon::CleanupId>* guardedExhaustionCleanups = nullptr;
    std::vector<moon::CleanupId>* guardedReturnCleanups = nullptr;
    if (guardedLoopCfg) {
        for (const auto& cleanup : guardedLoopCfg->cleanups) {
            if (!cleanup.guard) continue;
            guardedTailCleanups.push_back(cleanup.id);
            guardedNextUnread = cleanup.guard->nextUnread;
        }
        for (auto& block : guardedLoopCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration = dynamic_cast<moon::LetStmt*>(
                    operation.get());
                auto* transfer = declaration
                    ? dynamic_cast<moon::MoveExpr*>(
                          declaration->initializer.get()) : nullptr;
                if (transfer && !transfer->nextUnread.empty())
                    guardedElementTransfer = transfer;
            }
            const auto* condition = dynamic_cast<moon::BinaryExpr*>(
                block.terminator.operand.get());
            if (condition && condition->op == moon::Operator::Less)
                guardedExhaustionCleanups =
                    &block.terminator.secondary.cleanups;
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                !block.terminator.exitCleanups.empty())
                guardedReturnCleanups =
                    &block.terminator.exitCleanups;
        }
    }
    std::sort(guardedTailCleanups.begin(), guardedTailCleanups.end(),
              [](moon::CleanupId lhs, moon::CleanupId rhs) {
        return lhs.value < rhs.value;
    });
    auto sortedExhaustionCleanups = guardedExhaustionCleanups
        ? *guardedExhaustionCleanups
        : std::vector<moon::CleanupId>{};
    std::sort(sortedExhaustionCleanups.begin(),
              sortedExhaustionCleanups.end(),
              [](moon::CleanupId lhs, moon::CleanupId rhs) {
        return lhs.value < rhs.value;
    });
    const bool returnClosesTail = guardedReturnCleanups &&
        std::all_of(
            guardedTailCleanups.begin(), guardedTailCleanups.end(),
            [&](moon::CleanupId cleanup) {
                return std::find(
                    guardedReturnCleanups->begin(),
                    guardedReturnCleanups->end(), cleanup) !=
                    guardedReturnCleanups->end();
            });
    if (!guardedLoopCfg ||
        !cfgVerifier.verify(*guardedLoopCfg, module) ||
        guardedNextUnread.empty() || !guardedElementTransfer ||
        guardedElementTransfer->nextUnread != guardedNextUnread ||
        guardedTailCleanups.size() != 2 ||
        sortedExhaustionCleanups != guardedTailCleanups ||
        !returnClosesTail)
        return fail("move-only array loop did not lower to guarded tail cleanup state");
    guardedElementTransfer->nextUnread = {};
    if (cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("CFG verifier accepted an unguarded dynamic element move");
    guardedElementTransfer->nextUnread = guardedNextUnread;
    const auto savedGuardedExhaustion = *guardedExhaustionCleanups;
    guardedExhaustionCleanups->pop_back();
    if (cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("CFG verifier accepted an unread array-tail leak");
    *guardedExhaustionCleanups = savedGuardedExhaustion;
    if (!cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("restored guarded consuming-array CFG no longer verifies");

    auto standaloneBlockRoot = std::make_unique<moon::BlockStmt>();
    auto standaloneBlockUse = std::make_unique<moon::ExprStmt>();
    auto standaloneBlock = std::make_unique<moon::BlockExpr>();
    standaloneBlock->type = unitId;
    standaloneBlock->block = std::make_unique<moon::BlockStmt>();
    standaloneBlockUse->expr = std::move(standaloneBlock);
    standaloneBlockRoot->stmts.push_back(std::move(standaloneBlockUse));
    auto standaloneBlockCfg = cfgBuilder.build(
        std::move(standaloneBlockRoot), {},
        moon::RegionKind::Function, module);
    if (!standaloneBlockCfg ||
        !cfgVerifier.verify(*standaloneBlockCfg, module))
        return fail(
            "unit block expression did not expand into lexical CFG");

    auto ifExpressionRoot = std::make_unique<moon::BlockStmt>();
    auto ifExpressionUse = std::make_unique<moon::ExprStmt>();
    auto ifExpression = std::make_unique<moon::IfExpr>();
    ifExpression->type = unitId;
    auto outerCondition = std::make_unique<moon::BoolLiteralExpr>();
    outerCondition->value = true;
    outerCondition->type = boolId;
    ifExpression->cond = std::move(outerCondition);
    auto outerThen = std::make_unique<moon::BlockExpr>();
    outerThen->type = unitId;
    outerThen->block = std::make_unique<moon::BlockStmt>();
    ifExpression->thenExpr = std::move(outerThen);
    auto nestedIf = std::make_unique<moon::IfExpr>();
    nestedIf->type = unitId;
    auto nestedCondition = std::make_unique<moon::BoolLiteralExpr>();
    nestedCondition->value = false;
    nestedCondition->type = boolId;
    nestedIf->cond = std::move(nestedCondition);
    auto nestedThen = std::make_unique<moon::BlockExpr>();
    nestedThen->type = unitId;
    nestedThen->block = std::make_unique<moon::BlockStmt>();
    nestedIf->thenExpr = std::move(nestedThen);
    auto nestedElse = std::make_unique<moon::BlockExpr>();
    nestedElse->type = unitId;
    nestedElse->block = std::make_unique<moon::BlockStmt>();
    nestedIf->elseExpr = std::move(nestedElse);
    ifExpression->elseExpr = std::move(nestedIf);
    ifExpressionUse->expr = std::move(ifExpression);
    ifExpressionRoot->stmts.push_back(std::move(ifExpressionUse));
    auto ifExpressionCfg = cfgBuilder.build(
        std::move(ifExpressionRoot), {},
        moon::RegionKind::Function, module);
    size_t expressionBranches = 0;
    size_t unitResults = 0;
    if (ifExpressionCfg) {
        for (const auto& block : ifExpressionCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Branch)
                ++expressionBranches;
            for (const auto& operation : block.operations) {
                const auto* statement =
                    dynamic_cast<const moon::ExprStmt*>(operation.get());
                if (statement &&
                    dynamic_cast<const moon::UnitExpr*>(statement->expr.get()))
                    ++unitResults;
            }
        }
    }
    if (!ifExpressionCfg ||
        !cfgVerifier.verify(*ifExpressionCfg, module) ||
        expressionBranches != 2 || unitResults != 1)
        return fail(
            "nested unit if expression did not become canonical branch CFG");

    auto argumentControlRoot = std::make_unique<moon::BlockStmt>();
    auto argumentControlUse = std::make_unique<moon::ExprStmt>();
    auto argumentControlCall = std::make_unique<moon::CallExpr>();
    argumentControlCall->type = unitId;
    auto argumentConsumer = std::make_unique<moon::IdentifierExpr>();
    argumentConsumer->name = "unitConsumer";
    argumentConsumer->type = unitConsumerTypeId;
    argumentControlCall->callee = std::move(argumentConsumer);
    auto argumentIf = std::make_unique<moon::IfExpr>();
    argumentIf->type = unitId;
    auto argumentCondition = std::make_unique<moon::BoolLiteralExpr>();
    argumentCondition->value = true;
    argumentCondition->type = boolId;
    argumentIf->cond = std::move(argumentCondition);
    auto argumentThen = std::make_unique<moon::BlockExpr>();
    argumentThen->type = unitId;
    argumentThen->block = std::make_unique<moon::BlockStmt>();
    argumentIf->thenExpr = std::move(argumentThen);
    auto argumentElse = std::make_unique<moon::BlockExpr>();
    argumentElse->type = unitId;
    argumentElse->block = std::make_unique<moon::BlockStmt>();
    argumentIf->elseExpr = std::move(argumentElse);
    argumentControlCall->args.push_back(std::move(argumentIf));
    argumentControlUse->expr = std::move(argumentControlCall);
    argumentControlRoot->stmts.push_back(std::move(argumentControlUse));
    moon::Param unitConsumerParameter;
    unitConsumerParameter.name = "unitConsumer";
    unitConsumerParameter.type = unitConsumerTypeId;
    auto argumentControlCfg = cfgBuilder.build(
        std::move(argumentControlRoot), {unitConsumerParameter},
        moon::RegionKind::Function, module);
    if (!argumentControlCfg ||
        !cfgVerifier.verify(*argumentControlCfg, module)) {
        for (const auto& message : cfgBuilder.errors())
            std::cerr << message << '\n';
        for (const auto& message : cfgVerifier.errors())
            std::cerr << message << '\n';
        return fail(
            "if expression argument did not preserve ordered CFG evaluation");
    }

    auto invalidIfRoot = std::make_unique<moon::BlockStmt>();
    auto invalidIfUse = std::make_unique<moon::ExprStmt>();
    auto invalidIf = std::make_unique<moon::IfExpr>();
    invalidIf->type = i32Id;
    auto invalidCondition = std::make_unique<moon::BoolLiteralExpr>();
    invalidCondition->value = true;
    invalidCondition->type = boolId;
    invalidIf->cond = std::move(invalidCondition);
    auto invalidThen = std::make_unique<moon::BlockExpr>();
    invalidThen->type = i32Id;
    invalidThen->block = std::make_unique<moon::BlockStmt>();
    invalidIf->thenExpr = std::move(invalidThen);
    auto invalidElse = std::make_unique<moon::BlockExpr>();
    invalidElse->type = i32Id;
    invalidElse->block = std::make_unique<moon::BlockStmt>();
    invalidIf->elseExpr = std::move(invalidElse);
    invalidIfUse->expr = std::move(invalidIf);
    invalidIfRoot->stmts.push_back(std::move(invalidIfUse));
    if (cfgBuilder.build(
            std::move(invalidIfRoot), {},
            moon::RegionKind::Function, module))
        return fail(
            "CFG builder accepted a non-unit block-style if expression");

    auto tryRoot = std::make_unique<moon::BlockStmt>();
    auto tryBinding = std::make_unique<moon::LetStmt>();
    tryBinding->name = "unwrapped";
    tryBinding->type = i32Id;
    auto propagation = std::make_unique<moon::TryExpr>();
    propagation->type = i32Id;
    propagation->resultType = resultI32BoolId;
    propagation->propagatedResultType = resultI32BoolId;
    propagation->valueType = i32Id;
    propagation->errorType = boolId;
    propagation->propagatedErrorType = boolId;
    auto tryOperand = std::make_unique<moon::IdentifierExpr>();
    tryOperand->name = "input";
    tryOperand->type = resultI32BoolId;
    propagation->operand = std::move(tryOperand);
    tryBinding->initializer = std::move(propagation);
    tryRoot->stmts.push_back(std::move(tryBinding));
    moon::Param tryParameter;
    tryParameter.name = "input";
    tryParameter.type = resultI32BoolId;
    auto tryCfg = cfgBuilder.build(
        std::move(tryRoot), {tryParameter},
        moon::RegionKind::Function, module);
    moon::ResultConstructExpr* propagatedResult = nullptr;
    size_t trySwitches = 0;
    if (tryCfg) {
        for (auto& block : tryCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Switch)
                ++trySwitches;
            if (block.terminator.kind == moon::TerminatorKind::Return)
                if (auto* result =
                        dynamic_cast<moon::ResultConstructExpr*>(
                            block.terminator.operand.get()))
                    propagatedResult = result;
        }
    }
    if (!tryCfg || !cfgVerifier.verify(*tryCfg, module) ||
        trySwitches != 1 || !propagatedResult ||
        propagatedResult->isOk)
        return fail(
            "Try expression did not become Result switch and early return CFG");
    propagatedResult->isOk = true;
    if (cfgVerifier.verify(*tryCfg, module))
        return fail(
            "CFG verifier accepted a Result tag/payload mismatch");
    propagatedResult->isOk = false;
    if (!cfgVerifier.verify(*tryCfg, module))
        return fail(
            "restored Try expression CFG did not verify");

    moon::Module convertedTryModule;
    convertedTryModule.name = "canonical.try";
    const auto convertedI32Id = convertedTryModule.registerType(TyI32);
    const auto convertedBoolId = convertedTryModule.registerType(TyBool);
    const auto convertedStringId = convertedTryModule.registerType(TyString);
    auto sourceResultType = Type::makeResult(TyI32, TyString);
    auto targetResultType = Type::makeResult(TyI32, TyBool);
    const auto sourceResultId = convertedTryModule.registerType(
        sourceResultType);
    const auto targetResultId = convertedTryModule.registerType(
        targetResultType);
    auto fromType = Type::makeFunction(
        {TyString}, TyBool,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto fromTypeId = convertedTryModule.registerType(fromType);
    moon::DeclarationRecord fromRecord;
    fromRecord.id = "canonical.try::fn::from_string";
    fromRecord.familyId = fromRecord.id;
    fromRecord.symbolId = luna::identity::symbolIdFromCanonical(
        fromRecord.id);
    fromRecord.sourceName = "from_string";
    fromRecord.linkageName = "from_string";
    fromRecord.kind = moon::DeclarationKind::Function;
    fromRecord.type = fromTypeId;
    fromRecord.sysmeta = fromType->sysmeta;
    convertedTryModule.declarationTable.push_back(
        std::move(fromRecord));
    convertedTryModule.sealTypeTable();
    const auto* frozenFrom = convertedTryModule.findDeclarationById(
        "canonical.try::fn::from_string");
    if (!frozenFrom)
        return fail("converted Try fixture lost its From witness");
    const moon::DeclarationRef fromRef{
        frozenFrom->symbolId, frozenFrom->contractId};

    auto convertedTryRoot = std::make_unique<moon::BlockStmt>();
    auto convertedTryBinding = std::make_unique<moon::LetStmt>();
    convertedTryBinding->name = "converted";
    convertedTryBinding->type = convertedI32Id;
    auto convertedPropagation = std::make_unique<moon::TryExpr>();
    convertedPropagation->type = convertedI32Id;
    convertedPropagation->resultType = sourceResultId;
    convertedPropagation->propagatedResultType = targetResultId;
    convertedPropagation->valueType = convertedI32Id;
    convertedPropagation->errorType = convertedStringId;
    convertedPropagation->propagatedErrorType = convertedBoolId;
    convertedPropagation->errorConversion = fromRef;
    convertedPropagation->cleanups.push_back({
        "outer", luna::ownership::CleanupAction::Deallocate,
        convertedStringId});
    auto convertedOperand = std::make_unique<moon::IdentifierExpr>();
    convertedOperand->name = "fallible";
    convertedOperand->type = sourceResultId;
    convertedPropagation->operand = std::move(convertedOperand);
    convertedTryBinding->initializer = std::move(convertedPropagation);
    convertedTryRoot->stmts.push_back(std::move(convertedTryBinding));
    auto outerCleanup = std::make_unique<moon::FreeStmt>();
    outerCleanup->isImplicit = true;
    outerCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto outerCleanupOperand = std::make_unique<moon::IdentifierExpr>();
    outerCleanupOperand->name = "outer";
    outerCleanupOperand->type = convertedStringId;
    outerCleanup->operand = std::move(outerCleanupOperand);
    convertedTryRoot->stmts.push_back(std::move(outerCleanup));
    moon::Param fallibleParameter;
    fallibleParameter.name = "fallible";
    fallibleParameter.type = sourceResultId;
    fallibleParameter.usage = convertedTryModule.findType(
        sourceResultId)->sysmeta.resource.usage;
    moon::Param outerParameter;
    outerParameter.name = "outer";
    outerParameter.type = convertedStringId;
    outerParameter.usage = luna::ownership::Usage::Affine;
    auto convertedTryCfg = cfgBuilder.build(
        std::move(convertedTryRoot),
        {fallibleParameter, outerParameter},
        moon::RegionKind::Function, convertedTryModule);
    moon::Terminator* convertedFailure = nullptr;
    moon::CallExpr* convertedCall = nullptr;
    if (convertedTryCfg) {
        for (auto& block : convertedTryCfg->blocks) {
            auto* result = dynamic_cast<moon::ResultConstructExpr*>(
                block.terminator.operand.get());
            if (!result) continue;
            convertedFailure = &block.terminator;
            convertedCall = dynamic_cast<moon::CallExpr*>(
                result->payload.get());
        }
    }
    if (!convertedTryCfg) {
        for (const auto& message : cfgBuilder.errors())
            std::cerr << message << '\n';
        return fail(
            "converted Try did not build canonical CFG");
    }
    if (!cfgVerifier.verify(*convertedTryCfg, convertedTryModule)) {
        for (const auto& message : cfgVerifier.errors())
            std::cerr << message << '\n';
        return fail("converted Try CFG did not verify");
    }
    if (!convertedFailure || !convertedCall ||
        convertedCall->calleeRef != fromRef ||
        convertedFailure->exitCleanups.size() != 1)
        return fail(
            "converted Try did not preserve From and cleanup evidence");
    convertedFailure->exitCleanups.clear();
    if (cfgVerifier.verify(*convertedTryCfg, convertedTryModule))
        return fail(
            "CFG verifier accepted a converted Try without return cleanup");

    auto unresolvedStructured = std::make_unique<moon::BlockStmt>();
    auto unresolvedUse = std::make_unique<moon::ExprStmt>();
    auto unresolvedIdentifier = std::make_unique<moon::IdentifierExpr>();
    unresolvedIdentifier->name = "missing";
    unresolvedIdentifier->type = i32Id;
    unresolvedUse->expr = std::move(unresolvedIdentifier);
    unresolvedStructured->stmts.push_back(std::move(unresolvedUse));
    if (cfgBuilder.build(
            std::move(unresolvedStructured), {},
            moon::RegionKind::Function, module))
        return fail("CFG builder accepted an unresolved local identifier");

    auto movedStructured = std::make_unique<moon::BlockStmt>();
    auto sourceBinding = std::make_unique<moon::LetStmt>();
    sourceBinding->name = "source";
    sourceBinding->usage = luna::ownership::Usage::Affine;
    sourceBinding->type = stringId;
    auto sourceValue = std::make_unique<moon::StringLiteralExpr>();
    sourceValue->value = "move";
    sourceValue->type = stringId;
    sourceBinding->initializer = std::move(sourceValue);
    movedStructured->stmts.push_back(std::move(sourceBinding));
    auto destinationBinding = std::make_unique<moon::LetStmt>();
    destinationBinding->name = "destination";
    destinationBinding->usage = luna::ownership::Usage::Affine;
    destinationBinding->type = stringId;
    auto movedValue = std::make_unique<moon::MoveExpr>();
    auto movedIdentifier = std::make_unique<moon::IdentifierExpr>();
    movedIdentifier->name = "source";
    movedIdentifier->type = stringId;
    movedValue->operand = std::move(movedIdentifier);
    movedValue->type = stringId;
    destinationBinding->initializer = std::move(movedValue);
    movedStructured->stmts.push_back(std::move(destinationBinding));
    auto destinationCleanup = std::make_unique<moon::FreeStmt>();
    destinationCleanup->isImplicit = true;
    destinationCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto destinationIdentifier = std::make_unique<moon::IdentifierExpr>();
    destinationIdentifier->name = "destination";
    destinationIdentifier->type = stringId;
    destinationCleanup->operand = std::move(destinationIdentifier);
    movedStructured->stmts.push_back(std::move(destinationCleanup));
    auto movedCfg = cfgBuilder.build(
        std::move(movedStructured), {}, moon::RegionKind::Function, module);
    if (!movedCfg || !cfgVerifier.verify(*movedCfg, module) ||
        movedCfg->cleanups.size() != 2 ||
        movedCfg->blocks.front().terminator.exitCleanups !=
            std::vector<moon::CleanupId>{moon::CleanupId{1}})
        return fail("CFG cleanup dataflow lost a whole-place move transfer");
    movedCfg->blocks.front().terminator.exitCleanups = {moon::CleanupId{0}};
    if (cfgVerifier.verify(*movedCfg, module))
        return fail("CFG verifier accepted cleanup of a moved source place");

    moon::Module protocolModule;
    protocolModule.name = "canonical.iterator";
    auto iteratorState = Type::makeStruct(
        "IteratorState", {{"position", TyI32}},
        "canonical.iterator::IteratorState");
    auto iteratorReceiver = Type::makeReference(iteratorState, true);
    auto iteratorOption = Type::makeEnum(
        "Option", {{"None", {}}, {"Some", {TyI32}}},
        "canonical.iterator::Option<i32>");
    auto iteratorNextType = Type::makeFunction(
        {iteratorReceiver}, iteratorOption,
        {{luna::ownership::Relation::MutableBorrow,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    auto intoIteratorType = Type::makeFunction(
        {TyI32}, iteratorState,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    auto collected = Type::makeStruct(
        "Collected", {{"total", TyI32}},
        "canonical.iterator::Collected");
    auto collectBuilderReceiver = Type::makeReference(
        iteratorState, true);
    auto collectBeginType = Type::makeFunction(
        {}, iteratorState, {},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    auto collectPushType = Type::makeFunction(
        {collectBuilderReceiver, TyI32}, TyUnit,
        {{luna::ownership::Relation::MutableBorrow,
          luna::ownership::Usage::Copy},
         {luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    auto collectFinishType = Type::makeFunction(
        {iteratorState}, collected,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Affine});
    const auto iteratorStateId = protocolModule.registerType(iteratorState);
    const auto iteratorOptionId = protocolModule.registerType(iteratorOption);
    const auto iteratorNextTypeId = protocolModule.registerType(iteratorNextType);
    const auto intoIteratorTypeId = protocolModule.registerType(intoIteratorType);
    const auto protocolI32Id = protocolModule.registerType(TyI32);
    const auto protocolRangeId = protocolModule.registerType(rangeIterator);
    const auto collectedId = protocolModule.registerType(collected);
    const auto collectBeginTypeId = protocolModule.registerType(
        collectBeginType);
    const auto collectPushTypeId = protocolModule.registerType(
        collectPushType);
    const auto collectFinishTypeId = protocolModule.registerType(
        collectFinishType);
    protocolModule.registerType(TyBool);
    moon::DeclarationRecord nextRecord;
    nextRecord.id = "canonical.iterator::fn::next";
    nextRecord.familyId = nextRecord.id;
    nextRecord.symbolId = luna::identity::symbolIdFromCanonical(nextRecord.id);
    nextRecord.sourceName = "next";
    nextRecord.linkageName = "canonical_iterator_next";
    nextRecord.kind = moon::DeclarationKind::Function;
    nextRecord.type = iteratorNextTypeId;
    nextRecord.sysmeta = iteratorNextType->sysmeta;
    protocolModule.declarationTable.push_back(std::move(nextRecord));
    moon::DeclarationRecord intoRecord;
    intoRecord.id = "canonical.iterator::fn::into";
    intoRecord.familyId = intoRecord.id;
    intoRecord.symbolId = luna::identity::symbolIdFromCanonical(intoRecord.id);
    intoRecord.sourceName = "into";
    intoRecord.linkageName = "canonical_iterator_into";
    intoRecord.kind = moon::DeclarationKind::Function;
    intoRecord.type = intoIteratorTypeId;
    intoRecord.sysmeta = intoIteratorType->sysmeta;
    protocolModule.declarationTable.push_back(std::move(intoRecord));
    const auto addCollectDeclaration = [&](
        const std::string& name, const moon::TypeRef& type) {
        moon::DeclarationRecord record;
        record.id = "canonical.iterator::fn::" + name;
        record.familyId = record.id;
        record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
        record.sourceName = name;
        record.linkageName = "canonical_iterator_" + name;
        record.kind = moon::DeclarationKind::Function;
        record.type = type;
        protocolModule.declarationTable.push_back(std::move(record));
    };
    addCollectDeclaration("collect_begin", collectBeginTypeId);
    addCollectDeclaration("collect_push", collectPushTypeId);
    addCollectDeclaration("collect_finish", collectFinishTypeId);
    protocolModule.sealTypeTable();
    const auto* sealedNext = protocolModule.findDeclarationById(
        "canonical.iterator::fn::next");
    if (!sealedNext)
        return fail("iterator protocol fixture lost its declaration row");
    const auto* sealedInto = protocolModule.findDeclarationById(
        "canonical.iterator::fn::into");
    if (!sealedInto)
        return fail("IntoIterator fixture lost its declaration row");
    const auto* sealedCollectBegin = protocolModule.findDeclarationById(
        "canonical.iterator::fn::collect_begin");
    const auto* sealedCollectPush = protocolModule.findDeclarationById(
        "canonical.iterator::fn::collect_push");
    const auto* sealedCollectFinish = protocolModule.findDeclarationById(
        "canonical.iterator::fn::collect_finish");
    if (!sealedCollectBegin || !sealedCollectPush || !sealedCollectFinish ||
        protocolRangeId != rangeId || protocolI32Id != i32Id)
        return fail("collect protocol fixture lost its frozen witness rows");

    auto protocolStructured = std::make_unique<moon::BlockStmt>();
    auto protocolLoop = std::make_unique<moon::ForStmt>();
    protocolLoop->varName = "item";
    protocolLoop->bindingUsage = luna::ownership::Usage::Copy;
    protocolLoop->elementType = protocolI32Id;
    protocolLoop->protocolNext = {
        sealedNext->symbolId, sealedNext->contractId};
    protocolLoop->protocolIteratorType = iteratorStateId;
    protocolLoop->protocolOptionType = iteratorOptionId;
    protocolLoop->protocolNoneVariant = 0;
    protocolLoop->protocolSomeVariant = 1;
    auto iteratorIdentifier = std::make_unique<moon::IdentifierExpr>();
    iteratorIdentifier->name = "iterator";
    iteratorIdentifier->type = iteratorStateId;
    protocolLoop->iterable = std::move(iteratorIdentifier);
    protocolLoop->body = std::make_unique<moon::BlockStmt>();
    auto itemUse = std::make_unique<moon::ExprStmt>();
    auto itemIdentifier = std::make_unique<moon::IdentifierExpr>();
    itemIdentifier->name = "item";
    itemIdentifier->type = protocolI32Id;
    itemUse->expr = std::move(itemIdentifier);
    protocolLoop->body->stmts.push_back(std::move(itemUse));
    protocolStructured->stmts.push_back(std::move(protocolLoop));
    auto iteratorCleanup = std::make_unique<moon::FreeStmt>();
    iteratorCleanup->isImplicit = true;
    iteratorCleanup->action = cleanupActionForType(iteratorState);
    auto cleanedIterator = std::make_unique<moon::IdentifierExpr>();
    cleanedIterator->name = "iterator";
    cleanedIterator->type = iteratorStateId;
    iteratorCleanup->operand = std::move(cleanedIterator);
    protocolStructured->stmts.push_back(std::move(iteratorCleanup));
    moon::Param iteratorParameter;
    iteratorParameter.name = "iterator";
    iteratorParameter.type = iteratorStateId;
    iteratorParameter.usage = luna::ownership::Usage::Affine;
    auto protocolCfg = cfgBuilder.build(
        std::move(protocolStructured), {iteratorParameter},
        moon::RegionKind::Function, protocolModule);
    const bool protocolVerified = protocolCfg &&
        cfgVerifier.verify(*protocolCfg, protocolModule);
    if (!protocolVerified ||
        protocolCfg->blocks.size() != 5 || protocolCfg->locals.size() != 2 ||
        protocolCfg->blocks[1].terminator.kind !=
            moon::TerminatorKind::Switch ||
        protocolCfg->blocks[2].terminator.kind !=
            moon::TerminatorKind::Jump ||
        protocolCfg->blocks[2].terminator.primary.target != moon::BlockId{1}) {
        for (const auto& error : cfgBuilder.errors())
            std::cerr << error << '\n';
        for (const auto& error : cfgVerifier.errors())
            std::cerr << error << '\n';
        if (protocolCfg)
            std::cerr << "blocks=" << protocolCfg->blocks.size()
                      << " locals=" << protocolCfg->locals.size() << '\n';
        return fail("protocol for-loop did not normalize to switch/backedge CFG");
    }
    auto& protocolCases = protocolCfg->blocks[1].terminator.cases;
    if (protocolCases.size() != 2 ||
        protocolCases[1].bindings !=
            std::vector<moon::LocalId>{moon::LocalId{1}})
        return fail("protocol for-loop lost its per-iteration pattern local");
    protocolCases[1].bindings.clear();
    if (cfgVerifier.verify(*protocolCfg, protocolModule))
        return fail("CFG verifier accepted a missing iterator item binding");

    auto intoStructured = std::make_unique<moon::BlockStmt>();
    auto intoLoop = std::make_unique<moon::ForStmt>();
    intoLoop->varName = "item";
    intoLoop->bindingUsage = luna::ownership::Usage::Copy;
    intoLoop->elementType = protocolI32Id;
    intoLoop->protocolNext = {
        sealedNext->symbolId, sealedNext->contractId};
    intoLoop->protocolIteratorType = iteratorStateId;
    intoLoop->protocolOptionType = iteratorOptionId;
    intoLoop->protocolNoneVariant = 0;
    intoLoop->protocolSomeVariant = 1;
    intoLoop->protocolInto = {
        sealedInto->symbolId, sealedInto->contractId};
    intoLoop->protocolInputType = protocolI32Id;
    intoLoop->protocolStateName = "$iterator.state";
    intoLoop->protocolStateNeedsCleanup = true;
    intoLoop->protocolStateCleanup = cleanupActionForType(iteratorState);
    auto sourceIdentifier = std::make_unique<moon::IdentifierExpr>();
    sourceIdentifier->name = "source";
    sourceIdentifier->type = protocolI32Id;
    intoLoop->iterable = std::move(sourceIdentifier);
    intoLoop->body = std::make_unique<moon::BlockStmt>();
    intoStructured->stmts.push_back(std::move(intoLoop));
    moon::Param sourceParameter;
    sourceParameter.name = "source";
    sourceParameter.type = protocolI32Id;
    auto intoCfg = cfgBuilder.build(
        std::move(intoStructured), {sourceParameter},
        moon::RegionKind::Function, protocolModule);
    if (!intoCfg || !cfgVerifier.verify(*intoCfg, protocolModule) ||
        intoCfg->blocks.size() != 6 || intoCfg->locals.size() != 3 ||
        intoCfg->cleanups.size() != 1 ||
        intoCfg->blocks[2].terminator.cases[0].edge.cleanups !=
            std::vector<moon::CleanupId>{moon::CleanupId{0}})
        return fail("IntoIterator state did not receive a canonical exit cleanup");
    intoCfg->blocks[2].terminator.cases[0].edge.cleanups.clear();
    if (cfgVerifier.verify(*intoCfg, protocolModule))
        return fail("CFG verifier accepted a leaked IntoIterator state");

    auto recipeStructured = std::make_unique<moon::BlockStmt>();
    auto recipeLoop = std::make_unique<moon::ForStmt>();
    recipeLoop->varName = "value";
    recipeLoop->bindingUsage = luna::ownership::Usage::Copy;
    recipeLoop->elementType = i32Id;
    auto rangeCall = std::make_unique<moon::CallExpr>();
    rangeCall->iteratorOp = IteratorOp::Range;
    rangeCall->iteratorInputType = i32Id;
    rangeCall->iteratorOutputType = i32Id;
    rangeCall->type = rangeId;
    auto rangeCallee = std::make_unique<moon::IdentifierExpr>();
    rangeCallee->name = "range";
    rangeCall->callee = std::move(rangeCallee);
    auto rangeStart = std::make_unique<moon::IntLiteralExpr>();
    rangeStart->value = 0;
    rangeStart->type = i32Id;
    auto rangeEnd = std::make_unique<moon::IntLiteralExpr>();
    rangeEnd->value = 4;
    rangeEnd->type = i32Id;
    rangeCall->args.push_back(std::move(rangeStart));
    rangeCall->args.push_back(std::move(rangeEnd));
    auto takeCall = std::make_unique<moon::CallExpr>();
    takeCall->iteratorOp = IteratorOp::Take;
    takeCall->iteratorInputType = i32Id;
    takeCall->iteratorOutputType = i32Id;
    takeCall->type = rangeId;
    auto takeMember = std::make_unique<moon::FieldAccessExpr>();
    takeMember->field = "take";
    takeMember->object = std::move(rangeCall);
    takeCall->callee = std::move(takeMember);
    auto takeCount = std::make_unique<moon::IntLiteralExpr>();
    takeCount->value = 2;
    takeCount->type = i32Id;
    takeCall->args.push_back(std::move(takeCount));
    recipeLoop->iterable = std::move(takeCall);
    recipeLoop->body = std::make_unique<moon::BlockStmt>();
    auto recipeUse = std::make_unique<moon::ExprStmt>();
    auto recipeIdentifier = std::make_unique<moon::IdentifierExpr>();
    recipeIdentifier->name = "value";
    recipeIdentifier->type = i32Id;
    recipeUse->expr = std::move(recipeIdentifier);
    recipeLoop->body->stmts.push_back(std::move(recipeUse));
    recipeStructured->stmts.push_back(std::move(recipeLoop));
    auto recipeCfg = cfgBuilder.build(
        std::move(recipeStructured), {},
        moon::RegionKind::Function, module);
    if (!recipeCfg || !cfgVerifier.verify(*recipeCfg, module) ||
        recipeCfg->blocks.size() != 9 || recipeCfg->locals.size() != 4 ||
        recipeCfg->blocks[1].operations.size() != 3 ||
        recipeCfg->blocks[2].terminator.kind !=
            moon::TerminatorKind::Branch ||
        recipeCfg->blocks[5].terminator.kind !=
            moon::TerminatorKind::Branch ||
        recipeCfg->blocks[8].terminator.primary.target != moon::BlockId{2})
        return fail("range/take recipe did not expand to canonical CFG operations");

    const auto makeMaterializedRangeBinding =
        [&](const std::string& name, bool withTake = true) {
        auto binding = std::make_unique<moon::LetStmt>();
        binding->name = name;
        binding->type = rangeId;
        binding->usage = luna::ownership::Usage::Affine;
        binding->materializesIteratorRecipe = true;
        auto source = std::make_unique<moon::CallExpr>();
        source->iteratorOp = IteratorOp::Range;
        source->iteratorInputType = i32Id;
        source->iteratorOutputType = i32Id;
        source->type = rangeId;
        auto sourceCallee = std::make_unique<moon::IdentifierExpr>();
        sourceCallee->name = "range";
        source->callee = std::move(sourceCallee);
        auto start = std::make_unique<moon::IntLiteralExpr>();
        start->value = 1;
        start->type = i32Id;
        auto end = std::make_unique<moon::IntLiteralExpr>();
        end->value = 5;
        end->type = i32Id;
        source->args.push_back(std::move(start));
        source->args.push_back(std::move(end));
        if (!withTake) {
            binding->initializer = std::move(source);
            return binding;
        }
        auto take = std::make_unique<moon::CallExpr>();
        take->iteratorOp = IteratorOp::Take;
        take->iteratorInputType = i32Id;
        take->iteratorOutputType = i32Id;
        take->type = rangeId;
        auto takeMember = std::make_unique<moon::FieldAccessExpr>();
        takeMember->field = "take";
        takeMember->object = std::move(source);
        take->callee = std::move(takeMember);
        auto count = std::make_unique<moon::IntLiteralExpr>();
        count->value = 2;
        count->type = i32Id;
        take->args.push_back(std::move(count));
        binding->initializer = std::move(take);
        return binding;
    };
    const auto makeMaterializedRangeProgram = [&](size_t loopCount) {
        auto structured = std::make_unique<moon::BlockStmt>();
        structured->stmts.push_back(
            makeMaterializedRangeBinding("pending"));
        for (size_t loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
            auto loop = std::make_unique<moon::ForStmt>();
            loop->varName = "materializedValue" +
                std::to_string(loopIndex);
            loop->bindingUsage = luna::ownership::Usage::Copy;
            loop->elementType = i32Id;
            auto pending = std::make_unique<moon::IdentifierExpr>();
            pending->name = "pending";
            pending->type = rangeId;
            loop->iterable = std::move(pending);
            loop->body = std::make_unique<moon::BlockStmt>();
            structured->stmts.push_back(std::move(loop));
        }
        return structured;
    };
    auto materializedCfg = cfgBuilder.build(
        makeMaterializedRangeProgram(1), {},
        moon::RegionKind::Function, module);
    auto* materializedCursor = materializedCfg &&
            !materializedCfg->blocks.empty() &&
            !materializedCfg->blocks[0].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              materializedCfg->blocks[0].operations[0].get())
        : nullptr;
    auto* transferredCursor = materializedCfg &&
            materializedCfg->blocks.size() > 1 &&
            !materializedCfg->blocks[1].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              materializedCfg->blocks[1].operations[0].get())
        : nullptr;
    auto* cursorMove = transferredCursor
        ? dynamic_cast<moon::MoveExpr*>(
              transferredCursor->initializer.get())
        : nullptr;
    auto* movedCursor = cursorMove
        ? dynamic_cast<moon::IdentifierExpr*>(cursorMove->operand.get())
        : nullptr;
    bool retainedIteratorLocal = false;
    if (materializedCfg)
        for (const auto& local : materializedCfg->locals) {
            const auto* type = module.findType(local.type);
            retainedIteratorLocal = retainedIteratorLocal ||
                (type && type->kind == TypeKind::Iterator);
        }
    if (!materializedCfg || !cfgVerifier.verify(*materializedCfg, module) ||
        materializedCfg->blocks.size() != 9 ||
        materializedCfg->locals.size() != 6 ||
        materializedCfg->blocks[0].operations.size() != 3 ||
        materializedCfg->blocks[1].operations.size() != 2 ||
        !materializedCursor ||
        materializedCursor->usage != luna::ownership::Usage::Affine ||
        materializedCfg->locals[materializedCursor->local.value].kind !=
            moon::LocalKind::Synthetic ||
        !cursorMove || !movedCursor ||
        movedCursor->local != materializedCursor->local ||
        retainedIteratorLocal)
        return fail("materialized range did not erase to affine ordinary CFG state");

    auto savedCursorTransfer = std::move(transferredCursor->initializer);
    auto invalidCursorRead = std::make_unique<moon::IdentifierExpr>();
    invalidCursorRead->name = movedCursor->name;
    invalidCursorRead->local = movedCursor->local;
    invalidCursorRead->type = movedCursor->type;
    transferredCursor->initializer = std::move(invalidCursorRead);
    if (cfgVerifier.verify(*materializedCfg, module))
        return fail("CFG verifier accepted a copied materialized cursor");
    transferredCursor->initializer = std::move(savedCursorTransfer);
    if (!cfgVerifier.verify(*materializedCfg, module))
        return fail("restored materialized range CFG did not verify");

    auto reusedMaterializedCfg = cfgBuilder.build(
        makeMaterializedRangeProgram(2), {},
        moon::RegionKind::Function, module);
    if (!reusedMaterializedCfg ||
        cfgVerifier.verify(*reusedMaterializedCfg, module))
        return fail("CFG verifier accepted repeated materialized recipe consumption");

    const auto makeTerminal = [&](const std::string& recipeName, IteratorOp op,
            const moon::TypeRef& resultType,
            const moon::TypeRef& outputType) {
        auto call = std::make_unique<moon::CallExpr>();
        call->iteratorOp = op;
        call->iteratorInputType = i32Id;
        call->iteratorOutputType = outputType;
        call->type = resultType;
        auto member = std::make_unique<moon::FieldAccessExpr>();
        auto recipe = std::make_unique<moon::IdentifierExpr>();
        recipe->name = recipeName;
        recipe->type = rangeId;
        member->object = std::move(recipe);
        member->field = op == IteratorOp::Fold
            ? "fold"
            : (op == IteratorOp::ForEach
                   ? "for_each"
                   : (op == IteratorOp::Collect ? "collect" : "count"));
        call->callee = std::move(member);
        return call;
    };
    const auto makeDirectRangeTerminal = [&](
            IteratorOp op, const moon::TypeRef& resultType,
            const moon::TypeRef& outputType, bool withTake) {
        auto range = std::make_unique<moon::CallExpr>();
        range->iteratorOp = IteratorOp::Range;
        range->iteratorInputType = i32Id;
        range->iteratorOutputType = i32Id;
        range->type = rangeId;
        auto rangeCallee = std::make_unique<moon::IdentifierExpr>();
        rangeCallee->name = "range";
        range->callee = std::move(rangeCallee);
        auto start = std::make_unique<moon::IntLiteralExpr>();
        start->value = 1;
        start->type = i32Id;
        auto end = std::make_unique<moon::IntLiteralExpr>();
        end->value = 5;
        end->type = i32Id;
        range->args.push_back(std::move(start));
        range->args.push_back(std::move(end));

        std::unique_ptr<moon::Expr> recipe = std::move(range);
        if (withTake) {
            auto take = std::make_unique<moon::CallExpr>();
            take->iteratorOp = IteratorOp::Take;
            take->iteratorInputType = i32Id;
            take->iteratorOutputType = i32Id;
            take->type = rangeId;
            auto takeMember = std::make_unique<moon::FieldAccessExpr>();
            takeMember->field = "take";
            takeMember->object = std::move(recipe);
            take->callee = std::move(takeMember);
            auto count = std::make_unique<moon::IntLiteralExpr>();
            count->value = 3;
            count->type = i32Id;
            take->args.push_back(std::move(count));
            recipe = std::move(take);
        }

        auto terminal = std::make_unique<moon::CallExpr>();
        terminal->iteratorOp = op;
        terminal->iteratorInputType = i32Id;
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
    };

    auto statementConditionRoot = std::make_unique<moon::BlockStmt>();
    auto statementCondition = std::make_unique<moon::IfStmt>();
    auto terminalCondition = std::make_unique<moon::BinaryExpr>();
    terminalCondition->op = moon::Operator::Greater;
    terminalCondition->type = boolId;
    terminalCondition->lhs = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    auto conditionZero = std::make_unique<moon::IntLiteralExpr>();
    conditionZero->value = 0;
    conditionZero->type = i32Id;
    terminalCondition->rhs = std::move(conditionZero);
    statementCondition->cond = std::move(terminalCondition);
    statementCondition->thenBlock =
        std::make_unique<moon::BlockStmt>();
    statementCondition->elseBranch =
        std::make_unique<moon::BlockStmt>();
    statementConditionRoot->stmts.push_back(
        std::move(statementCondition));
    auto statementConditionCfg = cfgBuilder.build(
        std::move(statementConditionRoot), {},
        moon::RegionKind::Function, module);
    if (!statementConditionCfg ||
        !cfgVerifier.verify(*statementConditionCfg, module))
        return fail(
            "if statement condition did not normalize before branching");

    auto statementScrutineeRoot = std::make_unique<moon::BlockStmt>();
    auto statementScrutinee = std::make_unique<moon::MatchStmt>();
    statementScrutinee->matchedType = choiceId;
    auto terminalVariant =
        std::make_unique<moon::VariantConstructExpr>();
    terminalVariant->typeName = "Choice";
    terminalVariant->variantName = "Some";
    terminalVariant->constructedType = choiceId;
    terminalVariant->type = choiceId;
    terminalVariant->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    statementScrutinee->scrutinee = std::move(terminalVariant);
    moon::MatchArm statementNoneArm;
    statementNoneArm.variantName = "None";
    statementNoneArm.variantIndex = 0;
    statementNoneArm.body = std::make_unique<moon::BlockStmt>();
    statementScrutinee->arms.push_back(
        std::move(statementNoneArm));
    moon::MatchArm statementSomeArm;
    statementSomeArm.variantName = "Some";
    statementSomeArm.variantIndex = 1;
    statementSomeArm.bindings = {"item"};
    statementSomeArm.bindingTypes = {i32Id};
    statementSomeArm.bindingUsages = {
        luna::ownership::Usage::Copy};
    statementSomeArm.body = std::make_unique<moon::BlockStmt>();
    statementScrutinee->arms.push_back(
        std::move(statementSomeArm));
    statementScrutineeRoot->stmts.push_back(
        std::move(statementScrutinee));
    auto statementScrutineeCfg = cfgBuilder.build(
        std::move(statementScrutineeRoot), {},
        moon::RegionKind::Function, module);
    if (!statementScrutineeCfg ||
        !cfgVerifier.verify(*statementScrutineeCfg, module))
        return fail(
            "match statement scrutinee did not normalize before switching");

    auto repeatedConditionRoot = std::make_unique<moon::BlockStmt>();
    auto repeatedConditionLoop = std::make_unique<moon::WhileStmt>();
    auto repeatedCondition = std::make_unique<moon::BinaryExpr>();
    repeatedCondition->op = moon::Operator::Greater;
    repeatedCondition->type = boolId;
    repeatedCondition->lhs = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    auto repeatedZero = std::make_unique<moon::IntLiteralExpr>();
    repeatedZero->value = 0;
    repeatedZero->type = i32Id;
    repeatedCondition->rhs = std::move(repeatedZero);
    repeatedConditionLoop->cond = std::move(repeatedCondition);
    repeatedConditionLoop->body =
        std::make_unique<moon::BlockStmt>();
    repeatedConditionRoot->stmts.push_back(
        std::move(repeatedConditionLoop));
    auto repeatedConditionCfg = cfgBuilder.build(
        std::move(repeatedConditionRoot), {},
        moon::RegionKind::Function, module);
    const moon::RegionRecord* repeatedOuterLoop = nullptr;
    const moon::LocalRecord* repeatedCursor = nullptr;
    size_t repeatedHeaderPredecessors = 0;
    if (repeatedConditionCfg) {
        for (const auto& candidate : repeatedConditionCfg->regions)
            if (!repeatedOuterLoop &&
                candidate.kind == moon::RegionKind::Loop)
                repeatedOuterLoop = &candidate;
        for (const auto& local : repeatedConditionCfg->locals)
            if (local.name.rfind("$terminal.cursor.", 0) == 0)
                repeatedCursor = &local;
        if (repeatedOuterLoop)
            for (const auto& block : repeatedConditionCfg->blocks)
                if (block.terminator.kind ==
                        moon::TerminatorKind::Jump &&
                    block.terminator.primary.target ==
                        repeatedOuterLoop->entry)
                    ++repeatedHeaderPredecessors;
    }
    const auto* repeatedCursorScope = repeatedConditionCfg &&
            repeatedCursor
        ? repeatedConditionCfg->findScope(repeatedCursor->scope)
        : nullptr;
    const auto* repeatedEvaluationRegion = repeatedConditionCfg &&
            repeatedCursorScope
        ? repeatedConditionCfg->findRegion(
              repeatedCursorScope->region)
        : nullptr;
    if (!repeatedConditionCfg ||
        !cfgVerifier.verify(*repeatedConditionCfg, module) ||
        !repeatedOuterLoop || !repeatedCursorScope ||
        !repeatedEvaluationRegion ||
        repeatedEvaluationRegion->kind !=
            moon::RegionKind::Lexical ||
        repeatedEvaluationRegion->parent != repeatedOuterLoop->id ||
        repeatedCursorScope->parent != repeatedOuterLoop->scope ||
        repeatedHeaderPredecessors != 2)
        return fail(
            "repeated while condition did not rebuild synthetic state through its loop header");

    auto countStructured = std::make_unique<moon::BlockStmt>();
    countStructured->stmts.push_back(
        makeMaterializedRangeBinding("countPending"));
    auto countUse = std::make_unique<moon::ExprStmt>();
    auto sinkCall = std::make_unique<moon::CallExpr>();
    auto sink = std::make_unique<moon::IdentifierExpr>();
    sink->name = "sink";
    sink->type = actionTypeId;
    sinkCall->callee = std::move(sink);
    sinkCall->type = unitId;
    sinkCall->args.push_back(makeTerminal(
        "countPending", IteratorOp::Count, i32Id, i32Id));
    countUse->expr = std::move(sinkCall);
    countStructured->stmts.push_back(std::move(countUse));
    moon::Param sinkParameter;
    sinkParameter.name = "sink";
    sinkParameter.type = actionTypeId;
    auto countCfg = cfgBuilder.build(
        std::move(countStructured), {sinkParameter},
        moon::RegionKind::Function, module);
    bool countResultReachedSink = false;
    if (countCfg)
        for (const auto& block : countCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression =
                        dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call = dynamic_cast<const moon::CallExpr*>(
                            expression->expr.get());
                        call && call->iteratorOp == IteratorOp::None &&
                        call->args.size() == 1)
                        if (const auto* argument =
                                dynamic_cast<const moon::IdentifierExpr*>(
                                    call->args.front().get()))
                            countResultReachedSink = argument->name.rfind(
                                "$terminal.count.", 0) == 0;
    if (!countCfg || !cfgVerifier.verify(*countCfg, module) ||
        !countResultReachedSink)
        return fail("materialized count did not normalize before its outer call");

    auto foldStructured = std::make_unique<moon::BlockStmt>();
    foldStructured->stmts.push_back(
        makeMaterializedRangeBinding("foldPending"));
    auto foldBinding = std::make_unique<moon::LetStmt>();
    foldBinding->name = "folded";
    foldBinding->type = i32Id;
    foldBinding->usage = luna::ownership::Usage::Copy;
    auto transform = std::make_unique<moon::CallExpr>();
    transform->iteratorOp = IteratorOp::Map;
    transform->iteratorInputType = i32Id;
    transform->iteratorOutputType = i32Id;
    transform->type = rangeId;
    auto transformMember = std::make_unique<moon::FieldAccessExpr>();
    transformMember->field = "map";
    auto foldRecipe = std::make_unique<moon::IdentifierExpr>();
    foldRecipe->name = "foldPending";
    foldRecipe->type = rangeId;
    transformMember->object = std::move(foldRecipe);
    transform->callee = std::move(transformMember);
    auto transformFunction = std::make_unique<moon::IdentifierExpr>();
    transformFunction->name = "transform";
    transformFunction->type = lambdaTypeId;
    transform->args.push_back(std::move(transformFunction));
    auto foldTerminal = std::make_unique<moon::CallExpr>();
    foldTerminal->iteratorOp = IteratorOp::Fold;
    foldTerminal->iteratorInputType = i32Id;
    foldTerminal->iteratorOutputType = i32Id;
    foldTerminal->type = i32Id;
    auto foldMember = std::make_unique<moon::FieldAccessExpr>();
    foldMember->field = "fold";
    foldMember->object = std::move(transform);
    foldTerminal->callee = std::move(foldMember);
    auto initial = std::make_unique<moon::IntLiteralExpr>();
    initial->value = 0;
    initial->type = i32Id;
    foldTerminal->args.push_back(std::move(initial));
    auto reducer = std::make_unique<moon::IdentifierExpr>();
    reducer->name = "reducer";
    reducer->type = reducerTypeId;
    foldTerminal->args.push_back(std::move(reducer));
    foldBinding->initializer = std::move(foldTerminal);
    foldStructured->stmts.push_back(std::move(foldBinding));
    moon::Param transformParameter;
    transformParameter.name = "transform";
    transformParameter.type = lambdaTypeId;
    moon::Param reducerParameter;
    reducerParameter.name = "reducer";
    reducerParameter.type = reducerTypeId;
    auto foldCfg = cfgBuilder.build(
        std::move(foldStructured),
        {transformParameter, reducerParameter},
        moon::RegionKind::Function, module);
    bool foldedReadsAccumulator = false;
    size_t adapterOrder = static_cast<size_t>(-1);
    size_t accumulatorOrder = static_cast<size_t>(-1);
    size_t reducerOrder = static_cast<size_t>(-1);
    if (foldCfg)
        for (const auto& block : foldCfg->blocks)
            for (size_t operationIndex = 0;
                 operationIndex < block.operations.size(); ++operationIndex) {
                const auto& operation = block.operations[operationIndex];
                if (const auto* declaration =
                        dynamic_cast<const moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind(
                        "$terminal.adapter.", 0) == 0)
                    adapterOrder = operationIndex;
                else if (declaration && declaration->name.rfind(
                             "$terminal.fold.", 0) == 0)
                    accumulatorOrder = operationIndex;
                else if (declaration && declaration->name.rfind(
                             "$terminal.reducer.", 0) == 0)
                    reducerOrder = operationIndex;
                else if (declaration && declaration->name == "folded")
                    if (const auto* value =
                            dynamic_cast<const moon::IdentifierExpr*>(
                                declaration->initializer.get()))
                        foldedReadsAccumulator = value->name.rfind(
                            "$terminal.fold.", 0) == 0;
            }
    if (!foldCfg || !cfgVerifier.verify(*foldCfg, module) ||
        !foldedReadsAccumulator ||
        !(adapterOrder < accumulatorOrder &&
          accumulatorOrder < reducerOrder))
        return fail("materialized Copy fold did not normalize to accumulator CFG");

    auto directFoldStructured = std::make_unique<moon::BlockStmt>();
    auto directFoldUse = std::make_unique<moon::ExprStmt>();
    auto directFoldSink = std::make_unique<moon::CallExpr>();
    auto directSink = std::make_unique<moon::IdentifierExpr>();
    directSink->name = "sink";
    directSink->type = actionTypeId;
    directFoldSink->callee = std::move(directSink);
    directFoldSink->type = unitId;
    auto directFold = makeDirectRangeTerminal(
        IteratorOp::Fold, i32Id, i32Id, true);
    auto directInitial = std::make_unique<moon::IntLiteralExpr>();
    directInitial->value = 0;
    directInitial->type = i32Id;
    directFold->args.push_back(std::move(directInitial));
    auto directReducer = std::make_unique<moon::IdentifierExpr>();
    directReducer->name = "reducer";
    directReducer->type = reducerTypeId;
    directFold->args.push_back(std::move(directReducer));
    directFoldSink->args.push_back(std::move(directFold));
    directFoldUse->expr = std::move(directFoldSink);
    directFoldStructured->stmts.push_back(std::move(directFoldUse));
    auto directFoldCfg = cfgBuilder.build(
        std::move(directFoldStructured),
        {sinkParameter, reducerParameter},
        moon::RegionKind::Function, module);
    size_t directCursorOrder = static_cast<size_t>(-1);
    size_t directLimitOrder = static_cast<size_t>(-1);
    size_t directAdapterOrder = static_cast<size_t>(-1);
    size_t directAccumulatorOrder = static_cast<size_t>(-1);
    size_t directReducerOrder = static_cast<size_t>(-1);
    bool directFoldReachedSink = false;
    if (directFoldCfg) {
        const auto& entry = directFoldCfg->blocks.front();
        for (size_t index = 0; index < entry.operations.size(); ++index)
            if (const auto* declaration = dynamic_cast<const moon::LetStmt*>(
                    entry.operations[index].get())) {
                if (declaration->name.rfind("$terminal.cursor.", 0) == 0)
                    directCursorOrder = index;
                else if (declaration->name.rfind(
                             "$terminal.limit.", 0) == 0)
                    directLimitOrder = index;
                else if (declaration->name.rfind(
                             "$terminal.adapter.", 0) == 0)
                    directAdapterOrder = index;
                else if (declaration->name.rfind(
                             "$terminal.fold.", 0) == 0)
                    directAccumulatorOrder = index;
                else if (declaration->name.rfind(
                             "$terminal.reducer.", 0) == 0)
                    directReducerOrder = index;
            }
        for (const auto& block : directFoldCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression =
                        dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(
                                expression->expr.get());
                        call && call->args.size() == 1)
                        if (const auto* argument =
                                dynamic_cast<const moon::IdentifierExpr*>(
                                    call->args.front().get()))
                            directFoldReachedSink = argument->name.rfind(
                                "$terminal.fold.", 0) == 0;
    }
    if (!directFoldCfg ||
        !cfgVerifier.verify(*directFoldCfg, module) ||
        !directFoldReachedSink ||
        !(directCursorOrder < directLimitOrder &&
          directLimitOrder < directAdapterOrder &&
          directAdapterOrder < directAccumulatorOrder &&
          directAccumulatorOrder < directReducerOrder))
        return fail("direct Copy fold did not preserve receiver/terminal evaluation order");

    auto affineFoldStructured = std::make_unique<moon::BlockStmt>();
    auto affineFoldBinding = std::make_unique<moon::LetStmt>();
    affineFoldBinding->name = "affineFolded";
    affineFoldBinding->type = stringId;
    affineFoldBinding->usage = luna::ownership::Usage::Affine;
    auto affineFold = makeDirectRangeTerminal(
        IteratorOp::Fold, stringId, stringId, true);
    affineFold->returnUsage = luna::ownership::Usage::Affine;
    auto affineInitial = std::make_unique<moon::StringLiteralExpr>();
    affineInitial->value = "seed";
    affineInitial->type = stringId;
    affineFold->args.push_back(std::move(affineInitial));
    auto affineReducer = std::make_unique<moon::IdentifierExpr>();
    affineReducer->name = "affineReducer";
    affineReducer->type = affineReducerTypeId;
    affineFold->args.push_back(std::move(affineReducer));
    affineFoldBinding->initializer = std::move(affineFold);
    affineFoldStructured->stmts.push_back(
        std::move(affineFoldBinding));
    auto affineFoldCleanup = std::make_unique<moon::FreeStmt>();
    affineFoldCleanup->isImplicit = true;
    affineFoldCleanup->action = cleanupActionForType(TyString);
    auto affineFoldedIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    affineFoldedIdentifier->name = "affineFolded";
    affineFoldedIdentifier->type = stringId;
    affineFoldCleanup->operand = std::move(
        affineFoldedIdentifier);
    affineFoldStructured->stmts.push_back(
        std::move(affineFoldCleanup));
    moon::Param affineReducerParameter;
    affineReducerParameter.name = "affineReducer";
    affineReducerParameter.type = affineReducerTypeId;
    auto affineFoldCfg = cfgBuilder.build(
        std::move(affineFoldStructured),
        {affineReducerParameter},
        moon::RegionKind::Function, module);
    moon::LocalRecord* affineAccumulator = nullptr;
    moon::LetStmt* affineDestination = nullptr;
    moon::MoveExpr* affineFinalTransfer = nullptr;
    moon::AssignExpr* affineReplacement = nullptr;
    moon::CallExpr* affineReducerCall = nullptr;
    if (affineFoldCfg) {
        for (auto& local : affineFoldCfg->locals)
            if (local.name.rfind("$terminal.fold.", 0) == 0)
                affineAccumulator = &local;
        for (auto& block : affineFoldCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(
                        operation.get());
                    declaration && declaration->name == "affineFolded")
                    affineDestination = declaration;
                if (auto* expression = dynamic_cast<moon::ExprStmt*>(
                        operation.get()))
                    if (auto* assignment = dynamic_cast<moon::AssignExpr*>(
                            expression->expr.get());
                        assignment &&
                        assignment->op == moon::Operator::Assign) {
                        auto* call = dynamic_cast<moon::CallExpr*>(
                            assignment->rhs.get());
                        if (call && call->type == stringId) {
                            affineReplacement = assignment;
                            affineReducerCall = call;
                        }
                    }
            }
    }
    affineFinalTransfer = affineDestination
        ? dynamic_cast<moon::MoveExpr*>(
              affineDestination->initializer.get())
        : nullptr;
    auto* affineAccumulatorMove = affineReducerCall &&
            !affineReducerCall->args.empty()
        ? dynamic_cast<moon::MoveExpr*>(
              affineReducerCall->args.front().get())
        : nullptr;
    auto* movedAffineAccumulator = affineAccumulatorMove
        ? dynamic_cast<moon::IdentifierExpr*>(
              affineAccumulatorMove->operand.get())
        : nullptr;
    auto* finalAffineAccumulator = affineFinalTransfer
        ? dynamic_cast<moon::IdentifierExpr*>(
              affineFinalTransfer->operand.get())
        : nullptr;
    if (!affineFoldCfg ||
        !cfgVerifier.verify(*affineFoldCfg, module) ||
        !affineAccumulator ||
        affineAccumulator->kind != moon::LocalKind::Synthetic ||
        affineAccumulator->usage != luna::ownership::Usage::Affine ||
        !affineReplacement || !affineReducerCall ||
        affineReducerCall->returnUsage !=
            luna::ownership::Usage::Affine ||
        !movedAffineAccumulator || !finalAffineAccumulator ||
        movedAffineAccumulator->local != affineAccumulator->id ||
        finalAffineAccumulator->local != affineAccumulator->id ||
        affineFoldCfg->cleanups.size() != 2)
        return fail("affine fold did not normalize to a transfer/reinitialize CFG cycle");

    auto savedAffineArgument = std::move(
        affineReducerCall->args.front());
    auto copiedAffineAccumulator =
        std::make_unique<moon::IdentifierExpr>();
    copiedAffineAccumulator->name = affineAccumulator->name;
    copiedAffineAccumulator->local = affineAccumulator->id;
    copiedAffineAccumulator->type = affineAccumulator->type;
    affineReducerCall->args.front() = std::move(
        copiedAffineAccumulator);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail("CFG verifier accepted a copied affine fold accumulator");
    auto activeOverwrite = std::make_unique<moon::StringLiteralExpr>();
    activeOverwrite->value = "replacement";
    activeOverwrite->type = stringId;
    affineReducerCall->args.front() = std::move(activeOverwrite);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail(
            "CFG verifier accepted overwrite of an active affine fold accumulator");
    affineReducerCall->args.front() = std::move(savedAffineArgument);
    if (!cfgVerifier.verify(*affineFoldCfg, module))
        return fail("restored affine fold accumulator transfer did not verify");

    if (!affineDestination)
        return fail("affine fold destination disappeared from canonical CFG");
    auto savedDestinationInitializer = std::move(
        affineDestination->initializer);
    auto copiedFinalResult = std::make_unique<moon::IdentifierExpr>();
    copiedFinalResult->name = affineAccumulator->name;
    copiedFinalResult->local = affineAccumulator->id;
    copiedFinalResult->type = affineAccumulator->type;
    affineDestination->initializer = std::move(copiedFinalResult);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail("CFG verifier accepted a copied affine fold result");
    affineDestination->initializer = std::move(
        savedDestinationInitializer);
    affineFinalTransfer = dynamic_cast<moon::MoveExpr*>(
        affineDestination->initializer.get());
    if (!affineFinalTransfer)
        return fail("affine fold final transfer was not restored");
    if (!cfgVerifier.verify(*affineFoldCfg, module))
        return fail("restored affine fold result transfer did not verify");

    auto directCountStructured = std::make_unique<moon::BlockStmt>();
    auto directCountReturn = std::make_unique<moon::ReturnStmt>();
    directCountReturn->value = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    directCountStructured->stmts.push_back(
        std::move(directCountReturn));
    auto directCountCfg = cfgBuilder.build(
        std::move(directCountStructured), {},
        moon::RegionKind::Function, module);
    bool directCountReachedReturn = false;
    if (directCountCfg)
        for (const auto& block : directCountCfg->blocks)
            if (block.terminator.kind == moon::TerminatorKind::Return)
                if (const auto* value =
                        dynamic_cast<const moon::IdentifierExpr*>(
                            block.terminator.operand.get()))
                    directCountReachedReturn = value->name.rfind(
                        "$terminal.count.", 0) == 0;
    if (!directCountCfg ||
        !cfgVerifier.verify(*directCountCfg, module) ||
        !directCountReachedReturn)
        return fail("direct count did not normalize before return");

    auto siblingBoundary = std::make_unique<moon::BlockStmt>();
    auto siblingUse = std::make_unique<moon::ExprStmt>();
    auto siblingCall = std::make_unique<moon::CallExpr>();
    auto siblingReducer = std::make_unique<moon::IdentifierExpr>();
    siblingReducer->name = "reducer";
    siblingReducer->type = reducerTypeId;
    siblingCall->callee = std::move(siblingReducer);
    siblingCall->type = i32Id;
    auto earlierSibling = std::make_unique<moon::IntLiteralExpr>();
    earlierSibling->value = 9;
    earlierSibling->type = i32Id;
    siblingCall->args.push_back(std::move(earlierSibling));
    siblingCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    siblingUse->expr = std::move(siblingCall);
    siblingBoundary->stmts.push_back(std::move(siblingUse));
    auto siblingCfg = cfgBuilder.build(
        std::move(siblingBoundary), {reducerParameter},
        moon::RegionKind::Function, module);
    bool hoistedSiblingCallee = false;
    bool siblingTerminalReachedCall = false;
    if (siblingCfg)
        for (const auto& block : siblingCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* declaration =
                        dynamic_cast<const moon::LetStmt*>(
                            operation.get());
                    declaration && declaration->name.rfind(
                        "$expression.hoist.", 0) == 0) {
                    const auto* source =
                        dynamic_cast<const moon::IdentifierExpr*>(
                            declaration->initializer.get());
                    hoistedSiblingCallee = hoistedSiblingCallee ||
                        (source && source->name == "reducer");
                }
                const auto* expression =
                    dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = expression
                    ? dynamic_cast<const moon::CallExpr*>(
                          expression->expr.get())
                    : nullptr;
                if (!call || call->args.size() != 2) continue;
                const auto* result =
                    dynamic_cast<const moon::IdentifierExpr*>(
                        call->args[1].get());
                siblingTerminalReachedCall = result &&
                    result->name.rfind("$terminal.count.", 0) == 0;
            }
    if (!siblingCfg || !cfgVerifier.verify(*siblingCfg, module) ||
        !hoistedSiblingCallee || !siblingTerminalReachedCall)
        return fail(
            "Copy call sibling did not hoist before its terminal CFG");

    auto unitSiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto unitSiblingUse = std::make_unique<moon::ExprStmt>();
    auto unitSiblingCall = std::make_unique<moon::CallExpr>();
    unitSiblingCall->type = unitId;
    auto unitSiblingConsumer = std::make_unique<moon::IdentifierExpr>();
    unitSiblingConsumer->name = "unitOrderedConsumer";
    unitSiblingConsumer->type = unitOrderedConsumerTypeId;
    unitSiblingCall->callee = std::move(unitSiblingConsumer);
    auto earlierUnitCall = std::make_unique<moon::CallExpr>();
    earlierUnitCall->type = unitId;
    auto earlierUnitAction = std::make_unique<moon::IdentifierExpr>();
    earlierUnitAction->name = "action";
    earlierUnitAction->type = actionTypeId;
    earlierUnitCall->callee = std::move(earlierUnitAction);
    auto earlierUnitArgument = std::make_unique<moon::IntLiteralExpr>();
    earlierUnitArgument->value = 7;
    earlierUnitArgument->type = i32Id;
    earlierUnitCall->args.push_back(std::move(earlierUnitArgument));
    unitSiblingCall->args.push_back(std::move(earlierUnitCall));
    unitSiblingCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    unitSiblingUse->expr = std::move(unitSiblingCall);
    unitSiblingBoundary->stmts.push_back(std::move(unitSiblingUse));
    moon::Param unitOrderedConsumerParameter;
    unitOrderedConsumerParameter.name = "unitOrderedConsumer";
    unitOrderedConsumerParameter.type = unitOrderedConsumerTypeId;
    moon::Param unitSiblingActionParameter;
    unitSiblingActionParameter.name = "action";
    unitSiblingActionParameter.type = actionTypeId;
    auto unitSiblingCfg = cfgBuilder.build(
        std::move(unitSiblingBoundary),
        {unitOrderedConsumerParameter, unitSiblingActionParameter},
        moon::RegionKind::Function, module);
    size_t sequencedUnitCalls = 0;
    bool unitPlaceholderReachedParent = false;
    if (unitSiblingCfg)
        for (const auto& block : unitSiblingCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* statement =
                    dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = statement
                    ? dynamic_cast<const moon::CallExpr*>(
                          statement->expr.get())
                    : nullptr;
                if (!call) continue;
                const auto* callee =
                    dynamic_cast<const moon::IdentifierExpr*>(
                        call->callee.get());
                if (callee && callee->name == "action")
                    ++sequencedUnitCalls;
                if (call->args.size() != 2) continue;
                const auto* result =
                    dynamic_cast<const moon::IdentifierExpr*>(
                        call->args[1].get());
                unitPlaceholderReachedParent =
                    dynamic_cast<const moon::UnitExpr*>(
                        call->args[0].get()) &&
                    result && result->name.rfind(
                        "$terminal.count.", 0) == 0;
            }
    if (!unitSiblingCfg ||
        !cfgVerifier.verify(*unitSiblingCfg, module) ||
        sequencedUnitCalls != 1 || !unitPlaceholderReachedParent)
        return fail(
            "unit sibling did not sequence once before its terminal CFG");

    auto binarySiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto binaryReturn = std::make_unique<moon::ReturnStmt>();
    auto binary = std::make_unique<moon::BinaryExpr>();
    binary->op = moon::Operator::Add;
    binary->type = i32Id;
    auto earlierCall = std::make_unique<moon::CallExpr>();
    auto earlierReducer = std::make_unique<moon::IdentifierExpr>();
    earlierReducer->name = "reducer";
    earlierReducer->type = reducerTypeId;
    earlierCall->callee = std::move(earlierReducer);
    earlierCall->type = i32Id;
    for (int64_t value : {2, 3}) {
        auto argument = std::make_unique<moon::IntLiteralExpr>();
        argument->value = value;
        argument->type = i32Id;
        earlierCall->args.push_back(std::move(argument));
    }
    binary->lhs = std::move(earlierCall);
    binary->rhs = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    binaryReturn->value = std::move(binary);
    binarySiblingBoundary->stmts.push_back(std::move(binaryReturn));
    auto binarySiblingCfg = cfgBuilder.build(
        std::move(binarySiblingBoundary), {reducerParameter},
        moon::RegionKind::Function, module);
    bool hoistedBinarySibling = false;
    bool returnedNormalizedBinary = false;
    if (binarySiblingCfg)
        for (const auto& block : binarySiblingCfg->blocks) {
            for (const auto& operation : block.operations)
                if (const auto* declaration =
                        dynamic_cast<const moon::LetStmt*>(
                            operation.get());
                    declaration && declaration->name.rfind(
                        "$expression.hoist.", 0) == 0)
                    hoistedBinarySibling =
                        dynamic_cast<const moon::CallExpr*>(
                            declaration->initializer.get()) != nullptr;
            if (block.terminator.kind != moon::TerminatorKind::Return)
                continue;
            const auto* returned =
                dynamic_cast<const moon::BinaryExpr*>(
                    block.terminator.operand.get());
            const auto* lhs = returned
                ? dynamic_cast<const moon::IdentifierExpr*>(
                      returned->lhs.get())
                : nullptr;
            const auto* rhs = returned
                ? dynamic_cast<const moon::IdentifierExpr*>(
                      returned->rhs.get())
                : nullptr;
            returnedNormalizedBinary = lhs && rhs &&
                lhs->name.rfind("$expression.hoist.", 0) == 0 &&
                rhs->name.rfind("$terminal.count.", 0) == 0;
        }
    if (!binarySiblingCfg ||
        !cfgVerifier.verify(*binarySiblingCfg, module) ||
        !hoistedBinarySibling || !returnedNormalizedBinary)
        return fail(
            "Copy binary sibling did not preserve evaluation order across terminal CFG");

    auto affineOrderedBoundary = std::make_unique<moon::BlockStmt>();
    auto affineOrderedUse = std::make_unique<moon::ExprStmt>();
    auto affineOrderedCall = std::make_unique<moon::CallExpr>();
    auto affineOrderedReducer = std::make_unique<moon::IdentifierExpr>();
    affineOrderedReducer->name = "reducer";
    affineOrderedReducer->type = reducerTypeId;
    affineOrderedCall->callee = std::move(affineOrderedReducer);
    affineOrderedCall->type = i32Id;
    auto producedAffine = std::make_unique<moon::CallExpr>();
    auto affineProducer = std::make_unique<moon::IdentifierExpr>();
    affineProducer->name = "affineProducer";
    affineProducer->type = affineValueProducerTypeId;
    producedAffine->callee = std::move(affineProducer);
    producedAffine->type = i32Id;
    producedAffine->returnUsage = luna::ownership::Usage::Affine;
    affineOrderedCall->args.push_back(std::move(producedAffine));
    affineOrderedCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    affineOrderedUse->expr = std::move(affineOrderedCall);
    affineOrderedBoundary->stmts.push_back(
        std::move(affineOrderedUse));
    moon::Param affineProducerParameter;
    affineProducerParameter.name = "affineProducer";
    affineProducerParameter.type = affineValueProducerTypeId;
    auto affineOrderedCfg = cfgBuilder.build(
        std::move(affineOrderedBoundary),
        {reducerParameter, affineProducerParameter},
        moon::RegionKind::Function, module);
    moon::LetStmt* affineOrderedState = nullptr;
    moon::CallExpr* affineOrderedConsumer = nullptr;
    moon::MoveExpr* affineOrderedTransfer = nullptr;
    if (affineOrderedCfg)
        for (auto& block : affineOrderedCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration =
                        dynamic_cast<moon::LetStmt*>(
                            operation.get());
                    declaration && declaration->name.rfind(
                        "$expression.hoist.", 0) == 0 &&
                    declaration->usage ==
                        luna::ownership::Usage::Affine)
                    affineOrderedState = declaration;
                auto* statement =
                    dynamic_cast<moon::ExprStmt*>(operation.get());
                auto* call = statement
                    ? dynamic_cast<moon::CallExpr*>(
                          statement->expr.get())
                    : nullptr;
                if (call && call->args.size() == 2) {
                    affineOrderedConsumer = call;
                    affineOrderedTransfer =
                        dynamic_cast<moon::MoveExpr*>(
                            call->args.front().get());
                }
            }
    const auto* affineOrderedIdentifier = affineOrderedTransfer
        ? dynamic_cast<const moon::IdentifierExpr*>(
              affineOrderedTransfer->operand.get())
        : nullptr;
    if (!affineOrderedCfg ||
        !cfgVerifier.verify(*affineOrderedCfg, module) ||
        !affineOrderedState || !affineOrderedIdentifier ||
        affineOrderedIdentifier->local != affineOrderedState->local)
        return fail(
            "cleanup-free affine sibling did not transfer once across terminal CFG");
    auto preservedAffineTransfer = std::move(
        affineOrderedConsumer->args.front());
    affineOrderedConsumer->args.front() = std::move(
        affineOrderedTransfer->operand);
    if (cfgVerifier.verify(*affineOrderedCfg, module))
        return fail(
            "CFG verifier accepted a copied affine expression sibling");
    affineOrderedTransfer->operand = std::move(
        affineOrderedConsumer->args.front());
    affineOrderedConsumer->args.front() = std::move(
        preservedAffineTransfer);
    if (!cfgVerifier.verify(*affineOrderedCfg, module))
        return fail(
            "restored affine expression sibling CFG did not verify");

    moon::Param affineValueParameter;
    affineValueParameter.name = "affineValue";
    affineValueParameter.type = stringId;
    affineValueParameter.usage = luna::ownership::Usage::Affine;
    auto cleanupAffineBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupAffineBinding = std::make_unique<moon::LetStmt>();
    cleanupAffineBinding->name = "cleanupCombined";
    cleanupAffineBinding->type = stringId;
    cleanupAffineBinding->usage = luna::ownership::Usage::Affine;
    auto cleanupAffineCall = std::make_unique<moon::CallExpr>();
    auto cleanupAffineReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineReducer->name = "affineReducer";
    cleanupAffineReducer->type = affineReducerTypeId;
    cleanupAffineCall->callee = std::move(cleanupAffineReducer);
    cleanupAffineCall->type = stringId;
    cleanupAffineCall->returnUsage = luna::ownership::Usage::Affine;
    auto movedAffineValue = std::make_unique<moon::MoveExpr>();
    movedAffineValue->type = stringId;
    auto cleanupAffineSource = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineSource->name = "affineValue";
    cleanupAffineSource->type = stringId;
    movedAffineValue->operand = std::move(cleanupAffineSource);
    cleanupAffineCall->args.push_back(std::move(movedAffineValue));
    cleanupAffineCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    cleanupAffineBinding->initializer = std::move(cleanupAffineCall);
    cleanupAffineBoundary->stmts.push_back(
        std::move(cleanupAffineBinding));
    auto cleanupAffineRelease = std::make_unique<moon::FreeStmt>();
    cleanupAffineRelease->isImplicit = true;
    cleanupAffineRelease->action = cleanupActionForType(TyString);
    auto cleanupAffineResult = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineResult->name = "cleanupCombined";
    cleanupAffineResult->type = stringId;
    cleanupAffineRelease->operand = std::move(cleanupAffineResult);
    cleanupAffineBoundary->stmts.push_back(
        std::move(cleanupAffineRelease));
    auto cleanupAffineCfg = cfgBuilder.build(
        std::move(cleanupAffineBoundary),
        {affineReducerParameter, affineValueParameter},
        moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupAffineState = nullptr;
    bool cleanupAffineTransferred = false;
    if (cleanupAffineCfg) {
        for (const auto& local : cleanupAffineCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0)
                cleanupAffineState = &local;
        for (const auto& block : cleanupAffineCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* declaration =
                    dynamic_cast<const moon::LetStmt*>(operation.get());
                const auto* call = declaration &&
                        declaration->name == "cleanupCombined"
                    ? dynamic_cast<const moon::CallExpr*>(
                          declaration->initializer.get())
                    : nullptr;
                const auto* transfer = call && !call->args.empty()
                    ? dynamic_cast<const moon::MoveExpr*>(
                          call->args.front().get())
                    : nullptr;
                const auto* identifier = transfer
                    ? dynamic_cast<const moon::IdentifierExpr*>(
                          transfer->operand.get())
                    : nullptr;
                cleanupAffineTransferred = cleanupAffineTransferred ||
                    (cleanupAffineState && identifier &&
                     identifier->local == cleanupAffineState->id);
            }
    }
    bool cleanupAffineTracked = false;
    if (cleanupAffineCfg && cleanupAffineState)
        for (const auto& cleanup : cleanupAffineCfg->cleanups)
            cleanupAffineTracked = cleanupAffineTracked ||
                cleanup.place.root == cleanupAffineState->id;
    if (!cleanupAffineCfg ||
        !cfgVerifier.verify(*cleanupAffineCfg, module) ||
        !cleanupAffineState || !cleanupAffineTransferred ||
        !cleanupAffineTracked)
        return fail(
            "cleanup-bearing affine sibling did not survive a non-exiting terminal CFG");

    auto cleanupExitBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupExitBinding = std::make_unique<moon::LetStmt>();
    cleanupExitBinding->name = "cleanupTryCombined";
    cleanupExitBinding->type = stringId;
    cleanupExitBinding->usage = luna::ownership::Usage::Affine;
    auto cleanupExitCall = std::make_unique<moon::CallExpr>();
    auto cleanupExitReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupExitReducer->name = "affineReducer";
    cleanupExitReducer->type = affineReducerTypeId;
    cleanupExitCall->callee = std::move(cleanupExitReducer);
    cleanupExitCall->type = stringId;
    cleanupExitCall->returnUsage = luna::ownership::Usage::Affine;
    auto cleanupExitMove = std::make_unique<moon::MoveExpr>();
    cleanupExitMove->type = stringId;
    auto cleanupExitSource = std::make_unique<moon::IdentifierExpr>();
    cleanupExitSource->name = "affineValue";
    cleanupExitSource->type = stringId;
    cleanupExitMove->operand = std::move(cleanupExitSource);
    cleanupExitCall->args.push_back(std::move(cleanupExitMove));
    auto cleanupExitTry = std::make_unique<moon::TryExpr>();
    cleanupExitTry->type = i32Id;
    cleanupExitTry->resultType = resultI32BoolId;
    cleanupExitTry->propagatedResultType = resultI32BoolId;
    cleanupExitTry->valueType = i32Id;
    cleanupExitTry->errorType = boolId;
    cleanupExitTry->propagatedErrorType = boolId;
    auto cleanupExitInput = std::make_unique<moon::IdentifierExpr>();
    cleanupExitInput->name = "input";
    cleanupExitInput->type = resultI32BoolId;
    cleanupExitTry->operand = std::move(cleanupExitInput);
    cleanupExitCall->args.push_back(std::move(cleanupExitTry));
    cleanupExitBinding->initializer = std::move(cleanupExitCall);
    cleanupExitBoundary->stmts.push_back(
        std::move(cleanupExitBinding));
    auto cleanupExitRelease = std::make_unique<moon::FreeStmt>();
    cleanupExitRelease->isImplicit = true;
    cleanupExitRelease->action = cleanupActionForType(TyString);
    auto cleanupExitResult = std::make_unique<moon::IdentifierExpr>();
    cleanupExitResult->name = "cleanupTryCombined";
    cleanupExitResult->type = stringId;
    cleanupExitRelease->operand = std::move(cleanupExitResult);
    cleanupExitBoundary->stmts.push_back(
        std::move(cleanupExitRelease));
    auto cleanupExitCfg = cfgBuilder.build(
        std::move(cleanupExitBoundary),
        {affineReducerParameter, affineValueParameter, tryParameter},
        moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupExitState = nullptr;
    moon::CleanupId cleanupExitId;
    moon::Terminator* cleanupExitFailure = nullptr;
    if (cleanupExitCfg) {
        for (const auto& local : cleanupExitCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0)
                cleanupExitState = &local;
        if (cleanupExitState)
            for (const auto& cleanup : cleanupExitCfg->cleanups)
                if (cleanup.place.root == cleanupExitState->id) {
                    cleanupExitId = cleanup.id;
                    break;
                }
        for (auto& block : cleanupExitCfg->blocks) {
            auto* propagated =
                dynamic_cast<moon::ResultConstructExpr*>(
                    block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                propagated && !propagated->isOk)
                cleanupExitFailure = &block.terminator;
        }
    }
    if (!cleanupExitCfg ||
        !cfgVerifier.verify(*cleanupExitCfg, module) ||
        !cleanupExitState || cleanupExitId.empty() ||
        !cleanupExitFailure ||
        std::find(cleanupExitFailure->exitCleanups.begin(),
                  cleanupExitFailure->exitCleanups.end(),
                  cleanupExitId) ==
            cleanupExitFailure->exitCleanups.end())
        return fail(
            "Try failure did not clean an active affine expression sibling");
    const auto preservedCleanupExit = cleanupExitFailure->exitCleanups;
    cleanupExitFailure->exitCleanups.erase(
        std::remove(cleanupExitFailure->exitCleanups.begin(),
                    cleanupExitFailure->exitCleanups.end(),
                    cleanupExitId),
        cleanupExitFailure->exitCleanups.end());
    if (cfgVerifier.verify(*cleanupExitCfg, module))
        return fail(
            "CFG verifier accepted a Try edge without its affine sibling cleanup");
    cleanupExitFailure->exitCleanups = preservedCleanupExit;
    if (!cfgVerifier.verify(*cleanupExitCfg, module))
        return fail(
            "restored affine Try cleanup CFG did not verify");

    auto cleanupReturnBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupReturnUse = std::make_unique<moon::ExprStmt>();
    auto cleanupReturnCall = std::make_unique<moon::CallExpr>();
    auto cleanupReturnReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupReturnReducer->name = "affineReducer";
    cleanupReturnReducer->type = affineReducerTypeId;
    cleanupReturnCall->callee = std::move(cleanupReturnReducer);
    cleanupReturnCall->type = stringId;
    cleanupReturnCall->returnUsage = luna::ownership::Usage::Affine;
    auto cleanupReturnMove = std::make_unique<moon::MoveExpr>();
    cleanupReturnMove->type = stringId;
    auto cleanupReturnSource = std::make_unique<moon::IdentifierExpr>();
    cleanupReturnSource->name = "affineValue";
    cleanupReturnSource->type = stringId;
    cleanupReturnMove->operand = std::move(cleanupReturnSource);
    cleanupReturnCall->args.push_back(std::move(cleanupReturnMove));
    auto cleanupReturnBlock = std::make_unique<moon::BlockExpr>();
    cleanupReturnBlock->type = unitId;
    cleanupReturnBlock->block = std::make_unique<moon::BlockStmt>();
    auto cleanupReturn = std::make_unique<moon::ReturnStmt>();
    auto cleanupReturnValue = std::make_unique<moon::BoolLiteralExpr>();
    cleanupReturnValue->value = false;
    cleanupReturnValue->type = boolId;
    cleanupReturn->value = std::move(cleanupReturnValue);
    cleanupReturnBlock->block->stmts.push_back(
        std::move(cleanupReturn));
    cleanupReturnCall->args.push_back(std::move(cleanupReturnBlock));
    cleanupReturnUse->expr = std::move(cleanupReturnCall);
    cleanupReturnBoundary->stmts.push_back(
        std::move(cleanupReturnUse));
    auto cleanupReturnCfg = cfgBuilder.build(
        std::move(cleanupReturnBoundary),
        {affineReducerParameter, affineValueParameter},
        moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupReturnState = nullptr;
    moon::CleanupId cleanupReturnId;
    const moon::Terminator* cleanupReturnTerminator = nullptr;
    if (cleanupReturnCfg) {
        for (const auto& local : cleanupReturnCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0)
                cleanupReturnState = &local;
        if (cleanupReturnState)
            for (const auto& cleanup : cleanupReturnCfg->cleanups)
                if (cleanup.place.root == cleanupReturnState->id) {
                    cleanupReturnId = cleanup.id;
                    break;
                }
        for (const auto& block : cleanupReturnCfg->blocks)
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                dynamic_cast<const moon::BoolLiteralExpr*>(
                    block.terminator.operand.get()))
                cleanupReturnTerminator = &block.terminator;
    }
    if (!cleanupReturnCfg ||
        !cfgVerifier.verify(*cleanupReturnCfg, module) ||
        !cleanupReturnState || cleanupReturnId.empty() ||
        !cleanupReturnTerminator ||
        std::find(cleanupReturnTerminator->exitCleanups.begin(),
                  cleanupReturnTerminator->exitCleanups.end(),
                  cleanupReturnId) ==
            cleanupReturnTerminator->exitCleanups.end())
        return fail(
            "block return did not clean an active affine expression sibling");

    moon::Param linearReducerParameter;
    linearReducerParameter.name = "linearReducer";
    linearReducerParameter.type = linearReducerTypeId;
    moon::Param linearValueParameter;
    linearValueParameter.name = "linearValue";
    linearValueParameter.type = i32Id;
    linearValueParameter.usage = luna::ownership::Usage::Linear;
    auto linearOrderedBoundary = std::make_unique<moon::BlockStmt>();
    auto linearOrderedUse = std::make_unique<moon::ExprStmt>();
    auto linearOrderedCall = std::make_unique<moon::CallExpr>();
    auto linearOrderedReducer = std::make_unique<moon::IdentifierExpr>();
    linearOrderedReducer->name = "linearReducer";
    linearOrderedReducer->type = linearReducerTypeId;
    linearOrderedCall->callee = std::move(linearOrderedReducer);
    linearOrderedCall->type = i32Id;
    auto linearOrderedMove = std::make_unique<moon::MoveExpr>();
    linearOrderedMove->type = i32Id;
    auto linearOrderedSource = std::make_unique<moon::IdentifierExpr>();
    linearOrderedSource->name = "linearValue";
    linearOrderedSource->type = i32Id;
    linearOrderedMove->operand = std::move(linearOrderedSource);
    linearOrderedCall->args.push_back(std::move(linearOrderedMove));
    linearOrderedCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    linearOrderedUse->expr = std::move(linearOrderedCall);
    linearOrderedBoundary->stmts.push_back(
        std::move(linearOrderedUse));
    auto linearOrderedCfg = cfgBuilder.build(
        std::move(linearOrderedBoundary),
        {linearReducerParameter, linearValueParameter},
        moon::RegionKind::Function, module);
    const moon::LocalRecord* linearOrderedState = nullptr;
    const moon::MoveExpr* linearOrderedTransfer = nullptr;
    if (linearOrderedCfg) {
        for (const auto& local : linearOrderedCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0)
                linearOrderedState = &local;
        for (const auto& block : linearOrderedCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* statement =
                    dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = statement
                    ? dynamic_cast<const moon::CallExpr*>(
                          statement->expr.get())
                    : nullptr;
                if (call && call->args.size() == 2)
                    linearOrderedTransfer =
                        dynamic_cast<const moon::MoveExpr*>(
                            call->args.front().get());
            }
    }
    const auto* linearOrderedIdentifier = linearOrderedTransfer
        ? dynamic_cast<const moon::IdentifierExpr*>(
              linearOrderedTransfer->operand.get())
        : nullptr;
    if (!linearOrderedCfg ||
        !cfgVerifier.verify(*linearOrderedCfg, module) ||
        !linearOrderedState ||
        linearOrderedState->usage != luna::ownership::Usage::Linear ||
        !linearOrderedIdentifier ||
        linearOrderedIdentifier->local != linearOrderedState->id)
        return fail(
            "linear sibling did not transfer exactly once across a non-exiting CFG");

    auto linearExitBoundary = std::make_unique<moon::BlockStmt>();
    auto linearExitUse = std::make_unique<moon::ExprStmt>();
    auto linearExitCall = std::make_unique<moon::CallExpr>();
    auto linearExitReducer = std::make_unique<moon::IdentifierExpr>();
    linearExitReducer->name = "linearReducer";
    linearExitReducer->type = linearReducerTypeId;
    linearExitCall->callee = std::move(linearExitReducer);
    linearExitCall->type = i32Id;
    auto linearExitMove = std::make_unique<moon::MoveExpr>();
    linearExitMove->type = i32Id;
    auto linearExitSource = std::make_unique<moon::IdentifierExpr>();
    linearExitSource->name = "linearValue";
    linearExitSource->type = i32Id;
    linearExitMove->operand = std::move(linearExitSource);
    linearExitCall->args.push_back(std::move(linearExitMove));
    auto linearExitTry = std::make_unique<moon::TryExpr>();
    linearExitTry->type = i32Id;
    linearExitTry->resultType = resultI32BoolId;
    linearExitTry->propagatedResultType = resultI32BoolId;
    linearExitTry->valueType = i32Id;
    linearExitTry->errorType = boolId;
    linearExitTry->propagatedErrorType = boolId;
    auto linearExitInput = std::make_unique<moon::IdentifierExpr>();
    linearExitInput->name = "input";
    linearExitInput->type = resultI32BoolId;
    linearExitTry->operand = std::move(linearExitInput);
    linearExitCall->args.push_back(std::move(linearExitTry));
    linearExitUse->expr = std::move(linearExitCall);
    linearExitBoundary->stmts.push_back(std::move(linearExitUse));
    if (cfgBuilder.build(
            std::move(linearExitBoundary),
            {linearReducerParameter, linearValueParameter, tryParameter},
            moon::RegionKind::Function, module))
        return fail(
            "linear sibling crossed an early-exit CFG path");
    bool diagnosedLinearExit = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedLinearExit = diagnosedLinearExit ||
            message.find("linear expression sibling may cross an early-exit") !=
                std::string::npos;
    if (!diagnosedLinearExit)
        return fail(
            "linear sibling lost its early-exit diagnostic");

    auto affineSiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto affineSiblingUse = std::make_unique<moon::ExprStmt>();
    auto affineSiblingCall = std::make_unique<moon::CallExpr>();
    auto affineSiblingReducer = std::make_unique<moon::IdentifierExpr>();
    affineSiblingReducer->name = "affineReducer";
    affineSiblingReducer->type = affineReducerTypeId;
    affineSiblingCall->callee = std::move(affineSiblingReducer);
    affineSiblingCall->type = stringId;
    affineSiblingCall->returnUsage = luna::ownership::Usage::Affine;
    auto affineEarlierSibling = std::make_unique<moon::IdentifierExpr>();
    affineEarlierSibling->name = "affineValue";
    affineEarlierSibling->type = stringId;
    affineSiblingCall->args.push_back(std::move(affineEarlierSibling));
    affineSiblingCall->args.push_back(makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false));
    affineSiblingUse->expr = std::move(affineSiblingCall);
    affineSiblingBoundary->stmts.push_back(
        std::move(affineSiblingUse));
    if (cfgBuilder.build(
            std::move(affineSiblingBoundary),
            {affineReducerParameter, affineValueParameter},
            moon::RegionKind::Function, module))
        return fail(
            "move-only expression sibling entered Copy-only CFG hoisting");
    bool diagnosedAffineSibling = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedAffineSibling = diagnosedAffineSibling ||
            message.find("requires an explicit transfer") !=
                std::string::npos;
    if (!diagnosedAffineSibling)
        return fail(
            "move-only expression sibling lost its explicit hoisting boundary");

    auto shortCircuitBoundary = std::make_unique<moon::BlockStmt>();
    auto shortCircuitUse = std::make_unique<moon::ExprStmt>();
    auto shortCircuit = std::make_unique<moon::BinaryExpr>();
    shortCircuit->op = moon::Operator::LogicalAnd;
    shortCircuit->type = boolId;
    auto shortCircuitLeft = std::make_unique<moon::BoolLiteralExpr>();
    shortCircuitLeft->value = true;
    shortCircuitLeft->type = boolId;
    shortCircuit->lhs = std::move(shortCircuitLeft);
    auto shortCircuitRight = std::make_unique<moon::BinaryExpr>();
    shortCircuitRight->op = moon::Operator::Greater;
    shortCircuitRight->type = boolId;
    shortCircuitRight->lhs = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    auto zero = std::make_unique<moon::IntLiteralExpr>();
    zero->value = 0;
    zero->type = i32Id;
    shortCircuitRight->rhs = std::move(zero);
    shortCircuit->rhs = std::move(shortCircuitRight);
    shortCircuitUse->expr = std::move(shortCircuit);
    shortCircuitBoundary->stmts.push_back(
        std::move(shortCircuitUse));
    auto shortCircuitCfg = cfgBuilder.build(
        std::move(shortCircuitBoundary), {},
        moon::RegionKind::Function, module);
    moon::LetStmt* shortCircuitState = nullptr;
    moon::AssignExpr* shortCircuitAssignment = nullptr;
    if (shortCircuitCfg) {
        for (auto& block : shortCircuitCfg->blocks) {
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(
                        operation.get());
                    declaration && declaration->name.rfind(
                        "$short-circuit.", 0) == 0)
                    shortCircuitState = declaration;
                if (auto* statement = dynamic_cast<moon::ExprStmt*>(
                        operation.get()))
                    if (auto* assignment = dynamic_cast<moon::AssignExpr*>(
                            statement->expr.get());
                        assignment && assignment->lhs)
                        if (auto* destination =
                                dynamic_cast<moon::IdentifierExpr*>(
                                    assignment->lhs.get());
                            destination && destination->name.rfind(
                                "$short-circuit.", 0) == 0)
                            shortCircuitAssignment = assignment;
            }
        }
    }
    if (!shortCircuitCfg ||
        !cfgVerifier.verify(*shortCircuitCfg, module) ||
        !shortCircuitState || !shortCircuitAssignment)
        return fail(
            "short-circuit terminal did not become conditional CFG");
    auto preservedShortCircuitRhs = std::move(
        shortCircuitAssignment->rhs);
    auto forgedShortCircuit = std::make_unique<moon::BinaryExpr>();
    forgedShortCircuit->op = moon::Operator::LogicalOr;
    forgedShortCircuit->type = boolId;
    auto forgedLeft = std::make_unique<moon::BoolLiteralExpr>();
    forgedLeft->value = false;
    forgedLeft->type = boolId;
    forgedShortCircuit->lhs = std::move(forgedLeft);
    auto forgedRight = std::make_unique<moon::BoolLiteralExpr>();
    forgedRight->value = true;
    forgedRight->type = boolId;
    forgedShortCircuit->rhs = std::move(forgedRight);
    shortCircuitAssignment->rhs = std::move(forgedShortCircuit);
    if (cfgVerifier.verify(*shortCircuitCfg, module))
        return fail(
            "CFG verifier accepted a residual short-circuit expression");
    shortCircuitAssignment->rhs = std::move(
        preservedShortCircuitRhs);
    if (!cfgVerifier.verify(*shortCircuitCfg, module))
        return fail(
            "restored short-circuit CFG did not verify");

    auto inlineRecordBoundary = std::make_unique<moon::BlockStmt>();
    auto inlineRecordBinding = std::make_unique<moon::LetStmt>();
    inlineRecordBinding->name = "inlineSnapshot";
    inlineRecordBinding->type = inlineProductId;
    auto inlineRecord = std::make_unique<moon::RecordLiteralExpr>();
    inlineRecord->type = inlineProductId;
    moon::RecordLiteralExpr::Field inlineValueField;
    inlineValueField.name = "value";
    inlineValueField.value = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    inlineRecord->fields.push_back(std::move(inlineValueField));
    inlineRecordBinding->initializer = std::move(inlineRecord);
    inlineRecordBoundary->stmts.push_back(
        std::move(inlineRecordBinding));
    auto inlineRecordCfg = cfgBuilder.build(
        std::move(inlineRecordBoundary), {},
        moon::RegionKind::Function, module);
    bool normalizedInlineField = false;
    if (inlineRecordCfg)
        for (const auto& block : inlineRecordCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* declaration =
                    dynamic_cast<const moon::LetStmt*>(operation.get());
                const auto* value = declaration
                    ? dynamic_cast<const moon::RecordLiteralExpr*>(
                          declaration->initializer.get())
                    : nullptr;
                const auto* field = value && !value->fields.empty()
                    ? dynamic_cast<const moon::IdentifierExpr*>(
                          value->fields.front().value.get())
                    : nullptr;
                normalizedInlineField = normalizedInlineField ||
                    (field && field->name.rfind(
                        "$terminal.count.", 0) == 0);
            }
    if (!inlineRecordCfg ||
        !cfgVerifier.verify(*inlineRecordCfg, module) ||
        !normalizedInlineField)
        return fail(
            "inline record field did not preserve ordered CFG evaluation");

    auto recordTerminalBoundary = std::make_unique<moon::BlockStmt>();
    auto recordTerminalBinding = std::make_unique<moon::LetStmt>();
    recordTerminalBinding->name = "snapshot";
    recordTerminalBinding->type = productId;
    recordTerminalBinding->usage =
        module.findType(productId)->sysmeta.resource.usage;
    auto recordTerminal = std::make_unique<moon::RecordLiteralExpr>();
    recordTerminal->type = productId;
    moon::RecordLiteralExpr::Field valueField;
    valueField.name = "value";
    valueField.value = makeDirectRangeTerminal(
        IteratorOp::Count, i32Id, i32Id, false);
    recordTerminal->fields.push_back(std::move(valueField));
    recordTerminalBinding->initializer = std::move(recordTerminal);
    recordTerminalBoundary->stmts.push_back(
        std::move(recordTerminalBinding));
    auto recordTerminalRelease = std::make_unique<moon::FreeStmt>();
    recordTerminalRelease->isImplicit = true;
    recordTerminalRelease->action =
        luna::ownership::CleanupAction::Deallocate;
    auto recordTerminalResult = std::make_unique<moon::IdentifierExpr>();
    recordTerminalResult->name = "snapshot";
    recordTerminalResult->type = productId;
    recordTerminalRelease->operand = std::move(recordTerminalResult);
    recordTerminalBoundary->stmts.push_back(
        std::move(recordTerminalRelease));
    auto recordTerminalCfg = cfgBuilder.build(
        std::move(recordTerminalBoundary), {},
        moon::RegionKind::Function, module);
    const moon::AllocateStmt* recordAllocation = nullptr;
    const moon::InitAllocationExpr* recordInitialization = nullptr;
    bool retainedAllocatingRecord = false;
    if (recordTerminalCfg)
        for (const auto& block : recordTerminalCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* allocation =
                        dynamic_cast<const moon::AllocateStmt*>(
                            operation.get()))
                    recordAllocation = allocation;
                const auto* declaration =
                    dynamic_cast<const moon::LetStmt*>(operation.get());
                if (!declaration) continue;
                retainedAllocatingRecord = retainedAllocatingRecord ||
                    dynamic_cast<const moon::RecordLiteralExpr*>(
                        declaration->initializer.get());
                if (declaration->name == "snapshot")
                    recordInitialization =
                        dynamic_cast<const moon::InitAllocationExpr*>(
                            declaration->initializer.get());
            }
    const auto* recordInitializedField =
        recordInitialization && !recordInitialization->elements.empty()
        ? dynamic_cast<const moon::IdentifierExpr*>(
              recordInitialization->elements.front().value.get())
        : nullptr;
    if (!recordTerminalCfg ||
        !cfgVerifier.verify(*recordTerminalCfg, module) ||
        !recordAllocation || !recordInitialization ||
        recordInitialization->allocation != recordAllocation->local ||
        !recordInitializedField ||
        recordInitializedField->name.rfind("$terminal.count.", 0) != 0 ||
        retainedAllocatingRecord)
        return fail(
            "allocating record did not preserve allocation-before-terminal order");

    auto ownedRecordBoundary = std::make_unique<moon::BlockStmt>();
    auto ownedRecordBinding = std::make_unique<moon::LetStmt>();
    ownedRecordBinding->name = "ownedSnapshot";
    ownedRecordBinding->type = ownedProductId;
    ownedRecordBinding->usage =
        module.findType(ownedProductId)->sysmeta.resource.usage;
    auto ownedRecord = std::make_unique<moon::RecordLiteralExpr>();
    ownedRecord->type = ownedProductId;
    moon::RecordLiteralExpr::Field ownedField;
    ownedField.name = "owned";
    auto ownedFieldMove = std::make_unique<moon::MoveExpr>();
    ownedFieldMove->type = stringId;
    auto ownedFieldSource = std::make_unique<moon::IdentifierExpr>();
    ownedFieldSource->name = "affineValue";
    ownedFieldSource->type = stringId;
    ownedFieldMove->operand = std::move(ownedFieldSource);
    ownedField.value = std::move(ownedFieldMove);
    ownedRecord->fields.push_back(std::move(ownedField));
    moon::RecordLiteralExpr::Field fallibleField;
    fallibleField.name = "value";
    auto ownedRecordTry = std::make_unique<moon::TryExpr>();
    ownedRecordTry->type = i32Id;
    ownedRecordTry->resultType = resultI32BoolId;
    ownedRecordTry->propagatedResultType = resultI32BoolId;
    ownedRecordTry->valueType = i32Id;
    ownedRecordTry->errorType = boolId;
    ownedRecordTry->propagatedErrorType = boolId;
    auto ownedRecordInput = std::make_unique<moon::IdentifierExpr>();
    ownedRecordInput->name = "input";
    ownedRecordInput->type = resultI32BoolId;
    ownedRecordTry->operand = std::move(ownedRecordInput);
    fallibleField.value = std::move(ownedRecordTry);
    ownedRecord->fields.push_back(std::move(fallibleField));
    ownedRecordBinding->initializer = std::move(ownedRecord);
    ownedRecordBoundary->stmts.push_back(
        std::move(ownedRecordBinding));
    auto ownedRecordRelease = std::make_unique<moon::FreeStmt>();
    ownedRecordRelease->isImplicit = true;
    ownedRecordRelease->action = cleanupActionForType(ownedProduct);
    auto ownedRecordResult = std::make_unique<moon::IdentifierExpr>();
    ownedRecordResult->name = "ownedSnapshot";
    ownedRecordResult->type = ownedProductId;
    ownedRecordRelease->operand = std::move(ownedRecordResult);
    ownedRecordBoundary->stmts.push_back(
        std::move(ownedRecordRelease));
    auto ownedRecordCfg = cfgBuilder.build(
        std::move(ownedRecordBoundary),
        {affineValueParameter, tryParameter},
        moon::RegionKind::Function, module);
    moon::CleanupId ownedRecordRawCleanup;
    moon::CleanupId ownedRecordValueCleanup;
    const moon::Terminator* ownedRecordFailure = nullptr;
    if (ownedRecordCfg) {
        for (const auto& cleanup : ownedRecordCfg->cleanups) {
            const auto* local = ownedRecordCfg->findLocal(
                cleanup.place.root);
            if (local && local->kind == moon::LocalKind::Allocation)
                ownedRecordRawCleanup = cleanup.id;
            if (local && local->name.rfind(
                    "$expression.hoist.", 0) == 0)
                ownedRecordValueCleanup = cleanup.id;
        }
        for (const auto& block : ownedRecordCfg->blocks) {
            const auto* propagated =
                dynamic_cast<const moon::ResultConstructExpr*>(
                    block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                propagated && !propagated->isOk)
                ownedRecordFailure = &block.terminator;
        }
    }
    const auto rawCleanupPosition = ownedRecordFailure
        ? std::find(ownedRecordFailure->exitCleanups.begin(),
                    ownedRecordFailure->exitCleanups.end(),
                    ownedRecordRawCleanup)
        : std::vector<moon::CleanupId>::const_iterator{};
    const auto valueCleanupPosition = ownedRecordFailure
        ? std::find(ownedRecordFailure->exitCleanups.begin(),
                    ownedRecordFailure->exitCleanups.end(),
                    ownedRecordValueCleanup)
        : std::vector<moon::CleanupId>::const_iterator{};
    if (!ownedRecordCfg ||
        !cfgVerifier.verify(*ownedRecordCfg, module) ||
        ownedRecordRawCleanup.empty() ||
        ownedRecordValueCleanup.empty() || !ownedRecordFailure ||
        rawCleanupPosition == ownedRecordFailure->exitCleanups.end() ||
        valueCleanupPosition == ownedRecordFailure->exitCleanups.end() ||
        valueCleanupPosition > rawCleanupPosition)
        return fail(
            "partial struct initialization did not clean value before raw storage");

    auto heapTryBoundary = std::make_unique<moon::BlockStmt>();
    auto heapTryBinding = std::make_unique<moon::LetStmt>();
    heapTryBinding->name = "heapTryValue";
    heapTryBinding->type = i32Id;
    auto heapTry = std::make_unique<moon::HeapAllocExpr>();
    heapTry->type = i32Id;
    heapTry->allocatedType = i32Id;
    auto heapTryConstructor = std::make_unique<moon::CallExpr>();
    auto heapTryCallee = std::make_unique<moon::IdentifierExpr>();
    heapTryCallee->name = "i32";
    heapTryConstructor->callee = std::move(heapTryCallee);
    auto heapTryPropagation = std::make_unique<moon::TryExpr>();
    heapTryPropagation->type = i32Id;
    heapTryPropagation->resultType = resultI32BoolId;
    heapTryPropagation->propagatedResultType = resultI32BoolId;
    heapTryPropagation->valueType = i32Id;
    heapTryPropagation->errorType = boolId;
    heapTryPropagation->propagatedErrorType = boolId;
    auto heapTryInput = std::make_unique<moon::IdentifierExpr>();
    heapTryInput->name = "input";
    heapTryInput->type = resultI32BoolId;
    heapTryPropagation->operand = std::move(heapTryInput);
    heapTryConstructor->args.push_back(std::move(heapTryPropagation));
    heapTry->initializer = std::move(heapTryConstructor);
    heapTryBinding->initializer = std::move(heapTry);
    heapTryBoundary->stmts.push_back(std::move(heapTryBinding));
    auto heapTryRelease = std::make_unique<moon::FreeStmt>();
    heapTryRelease->isImplicit = true;
    heapTryRelease->action =
        luna::ownership::CleanupAction::Deallocate;
    auto heapTryResult = std::make_unique<moon::IdentifierExpr>();
    heapTryResult->name = "heapTryValue";
    heapTryResult->type = i32Id;
    heapTryRelease->operand = std::move(heapTryResult);
    heapTryBoundary->stmts.push_back(std::move(heapTryRelease));
    auto heapTryCfg = cfgBuilder.build(
        std::move(heapTryBoundary), {tryParameter},
        moon::RegionKind::Function, module);
    const moon::LocalRecord* heapRawLocal = nullptr;
    moon::CleanupId heapRawCleanup;
    moon::Terminator* heapFailure = nullptr;
    moon::InitAllocationExpr* heapInitialization = nullptr;
    if (heapTryCfg) {
        for (const auto& local : heapTryCfg->locals)
            if (local.kind == moon::LocalKind::Allocation)
                heapRawLocal = &local;
        if (heapRawLocal)
            for (const auto& cleanup : heapTryCfg->cleanups)
                if (cleanup.place.root == heapRawLocal->id &&
                    cleanup.kind == moon::CleanupKind::Allocation) {
                    heapRawCleanup = cleanup.id;
                    break;
                }
        for (auto& block : heapTryCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration =
                    dynamic_cast<moon::LetStmt*>(operation.get());
                if (declaration && declaration->name == "heapTryValue")
                    heapInitialization =
                        dynamic_cast<moon::InitAllocationExpr*>(
                            declaration->initializer.get());
            }
            const auto* propagated =
                dynamic_cast<const moon::ResultConstructExpr*>(
                    block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                propagated && !propagated->isOk)
                heapFailure = &block.terminator;
        }
    }
    if (!heapTryCfg || !cfgVerifier.verify(*heapTryCfg, module) ||
        !heapRawLocal || heapRawCleanup.empty() || !heapFailure ||
        !heapInitialization ||
        std::find(heapFailure->exitCleanups.begin(),
                  heapFailure->exitCleanups.end(), heapRawCleanup) ==
            heapFailure->exitCleanups.end())
        return fail(
            "heap initializer early exit did not release raw allocation");
    const auto preservedHeapFailureCleanups = heapFailure->exitCleanups;
    heapFailure->exitCleanups.erase(
        std::remove(heapFailure->exitCleanups.begin(),
                    heapFailure->exitCleanups.end(), heapRawCleanup),
        heapFailure->exitCleanups.end());
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail(
            "CFG verifier accepted a leaking heap initializer failure edge");
    heapFailure->exitCleanups = preservedHeapFailureCleanups;
    if (!cfgVerifier.verify(*heapTryCfg, module))
        return fail(
            "restored heap initializer cleanup CFG did not verify");
    const auto preservedHeapAllocation = heapInitialization->allocation;
    heapInitialization->allocation = moon::LocalId{999};
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail(
            "CFG verifier accepted an initialization with no allocation identity");
    heapInitialization->allocation = preservedHeapAllocation;
    auto& heapRawCleanupRecord =
        heapTryCfg->cleanups[heapRawCleanup.value];
    heapRawCleanupRecord.kind = moon::CleanupKind::Value;
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail(
            "CFG verifier accepted raw storage with a value cleanup");
    heapRawCleanupRecord.kind = moon::CleanupKind::Allocation;
    if (!cfgVerifier.verify(*heapTryCfg, module))
        return fail(
            "restored heap allocation identity did not verify");

    auto discardedAllocationBoundary =
        std::make_unique<moon::BlockStmt>();
    auto discardedAllocationUse = std::make_unique<moon::ExprStmt>();
    auto discardedAllocation = std::make_unique<moon::HeapAllocExpr>();
    discardedAllocation->type = i32Id;
    discardedAllocation->allocatedType = i32Id;
    auto discardedConstructor = std::make_unique<moon::CallExpr>();
    auto discardedCallee = std::make_unique<moon::IdentifierExpr>();
    discardedCallee->name = "i32";
    discardedConstructor->callee = std::move(discardedCallee);
    auto discardedValue = std::make_unique<moon::IntLiteralExpr>();
    discardedValue->value = 1;
    discardedValue->type = i32Id;
    discardedConstructor->args.push_back(std::move(discardedValue));
    discardedAllocation->initializer = std::move(discardedConstructor);
    discardedAllocationUse->expr = std::move(discardedAllocation);
    discardedAllocationBoundary->stmts.push_back(
        std::move(discardedAllocationUse));
    if (cfgBuilder.build(
            std::move(discardedAllocationBoundary), {},
            moon::RegionKind::Function, module))
        return fail("CFG builder accepted a discarded owning allocation");
    bool diagnosedDiscardedAllocation = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedDiscardedAllocation = diagnosedDiscardedAllocation ||
            message.find("allocation result cannot be discarded") !=
                std::string::npos;
    if (!diagnosedDiscardedAllocation)
        return fail("discarded allocation lost its ownership diagnostic");

    auto forEachStructured = std::make_unique<moon::BlockStmt>();
    forEachStructured->stmts.push_back(
        makeMaterializedRangeBinding("forEachPending", false));
    auto forEachUse = std::make_unique<moon::ExprStmt>();
    auto forEachTerminal = makeTerminal(
        "forEachPending", IteratorOp::ForEach, unitId, unitId);
    auto action = std::make_unique<moon::IdentifierExpr>();
    action->name = "action";
    action->type = actionTypeId;
    forEachTerminal->args.push_back(std::move(action));
    forEachUse->expr = std::move(forEachTerminal);
    forEachStructured->stmts.push_back(std::move(forEachUse));
    moon::Param actionParameter;
    actionParameter.name = "action";
    actionParameter.type = actionTypeId;
    auto forEachCfg = cfgBuilder.build(
        std::move(forEachStructured), {actionParameter},
        moon::RegionKind::Function, module);
    bool retainedForEachTerminal = false;
    if (forEachCfg)
        for (const auto& block : forEachCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression =
                        dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call = dynamic_cast<const moon::CallExpr*>(
                            expression->expr.get()))
                        retainedForEachTerminal = retainedForEachTerminal ||
                            call->iteratorOp == IteratorOp::ForEach;
    if (!forEachCfg || !cfgVerifier.verify(*forEachCfg, module) ||
        retainedForEachTerminal)
        return fail("materialized for_each did not normalize to loop body calls");

    auto directForEachStructured = std::make_unique<moon::BlockStmt>();
    auto directForEachUse = std::make_unique<moon::ExprStmt>();
    auto directForEach = makeDirectRangeTerminal(
        IteratorOp::ForEach, unitId, unitId, true);
    auto directAction = std::make_unique<moon::IdentifierExpr>();
    directAction->name = "action";
    directAction->type = actionTypeId;
    directForEach->args.push_back(std::move(directAction));
    directForEachUse->expr = std::move(directForEach);
    directForEachStructured->stmts.push_back(
        std::move(directForEachUse));
    auto directForEachCfg = cfgBuilder.build(
        std::move(directForEachStructured), {actionParameter},
        moon::RegionKind::Function, module);
    bool retainedDirectForEachTerminal = false;
    bool directForEachActionLocal = false;
    if (directForEachCfg)
        for (const auto& block : directForEachCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* declaration =
                        dynamic_cast<const moon::LetStmt*>(operation.get()))
                    directForEachActionLocal =
                        directForEachActionLocal ||
                        declaration->name.rfind(
                            "$terminal.action.", 0) == 0;
                if (const auto* expression =
                        dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(
                                expression->expr.get()))
                        retainedDirectForEachTerminal =
                            retainedDirectForEachTerminal ||
                            call->iteratorOp == IteratorOp::ForEach;
            }
    if (!directForEachCfg ||
        !cfgVerifier.verify(*directForEachCfg, module) ||
        !directForEachActionLocal || retainedDirectForEachTerminal)
        return fail("direct for_each did not normalize to body calls");

    auto collectStructured = std::make_unique<moon::BlockStmt>();
    collectStructured->stmts.push_back(
        makeMaterializedRangeBinding("collectPending", false));
    auto collectBinding = std::make_unique<moon::LetStmt>();
    collectBinding->name = "collected";
    collectBinding->type = collectedId;
    collectBinding->usage = luna::ownership::Usage::Affine;
    auto collectTerminal = makeTerminal(
        "collectPending", IteratorOp::Collect,
        collectedId, collectedId);
    collectTerminal->returnUsage = luna::ownership::Usage::Affine;
    collectTerminal->iteratorCollectTargetType = collectedId;
    collectTerminal->iteratorCollectBuilderType = iteratorStateId;
    collectTerminal->iteratorCollectBegin = {
        sealedCollectBegin->symbolId, sealedCollectBegin->contractId};
    collectTerminal->iteratorCollectPush = {
        sealedCollectPush->symbolId, sealedCollectPush->contractId};
    collectTerminal->iteratorCollectFinish = {
        sealedCollectFinish->symbolId, sealedCollectFinish->contractId};
    collectBinding->initializer = std::move(collectTerminal);
    collectStructured->stmts.push_back(std::move(collectBinding));
    auto collectCleanup = std::make_unique<moon::FreeStmt>();
    collectCleanup->isImplicit = true;
    collectCleanup->action = cleanupActionForType(collected);
    auto collectedIdentifier = std::make_unique<moon::IdentifierExpr>();
    collectedIdentifier->name = "collected";
    collectedIdentifier->type = collectedId;
    collectCleanup->operand = std::move(collectedIdentifier);
    collectStructured->stmts.push_back(std::move(collectCleanup));
    auto collectCfg = cfgBuilder.build(
        std::move(collectStructured), {},
        moon::RegionKind::Function, protocolModule);
    moon::LocalId collectBuilderLocal;
    moon::LocalId collectResultLocal;
    moon::CallExpr* loweredPush = nullptr;
    moon::CallExpr* loweredFinish = nullptr;
    moon::LetStmt* loweredUserCollect = nullptr;
    bool retainedCollectTerminal = false;
    if (collectCfg)
        for (auto& block : collectCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(
                        operation.get())) {
                    if (declaration->name.rfind(
                            "$terminal.collect.builder.", 0) == 0)
                        collectBuilderLocal = declaration->local;
                    else if (declaration->name.rfind(
                                 "$terminal.collect.result.", 0) == 0) {
                        collectResultLocal = declaration->local;
                        loweredFinish = dynamic_cast<moon::CallExpr*>(
                            declaration->initializer.get());
                    } else if (declaration->name == "collected") {
                        loweredUserCollect = declaration;
                    }
                } else if (auto* expression = dynamic_cast<moon::ExprStmt*>(
                               operation.get())) {
                    auto* call = dynamic_cast<moon::CallExpr*>(
                        expression->expr.get());
                    retainedCollectTerminal = retainedCollectTerminal ||
                        (call && call->iteratorOp == IteratorOp::Collect);
                    if (call && call->calleeRef ==
                            moon::DeclarationRef{
                                sealedCollectPush->symbolId,
                                sealedCollectPush->contractId})
                        loweredPush = call;
                }
            }
    auto* builderBorrow = loweredPush && loweredPush->args.size() == 2
        ? dynamic_cast<moon::BorrowExpr*>(loweredPush->args[0].get())
        : nullptr;
    auto* borrowedBuilder = builderBorrow
        ? dynamic_cast<moon::IdentifierExpr*>(
              builderBorrow->operand.get())
        : nullptr;
    auto* finishMove = loweredFinish && loweredFinish->args.size() == 1
        ? dynamic_cast<moon::MoveExpr*>(loweredFinish->args[0].get())
        : nullptr;
    auto* finishedBuilder = finishMove
        ? dynamic_cast<moon::IdentifierExpr*>(finishMove->operand.get())
        : nullptr;
    auto* resultMove = loweredUserCollect
        ? dynamic_cast<moon::MoveExpr*>(
              loweredUserCollect->initializer.get())
        : nullptr;
    auto* transferredResult = resultMove
        ? dynamic_cast<moon::IdentifierExpr*>(resultMove->operand.get())
        : nullptr;
    if (!collectCfg || !cfgVerifier.verify(*collectCfg, protocolModule) ||
        collectBuilderLocal.empty() || collectResultLocal.empty() ||
        !builderBorrow || !builderBorrow->isMutable || !borrowedBuilder ||
        borrowedBuilder->local != collectBuilderLocal ||
        !finishMove || !finishedBuilder ||
        finishedBuilder->local != collectBuilderLocal ||
        !resultMove || !transferredResult ||
        transferredResult->local != collectResultLocal ||
        retainedCollectTerminal || collectCfg->cleanups.size() != 3) {
        for (const auto& error : cfgBuilder.errors())
            std::cerr << error << '\n';
        for (const auto& error : cfgVerifier.errors())
            std::cerr << error << '\n';
        if (collectCfg)
            std::cerr << "collect blocks=" << collectCfg->blocks.size()
                      << " locals=" << collectCfg->locals.size()
                      << " cleanups=" << collectCfg->cleanups.size()
                      << " builder=" << collectBuilderLocal.value
                      << " result=" << collectResultLocal.value
                      << " push=" << (loweredPush != nullptr)
                      << " finish=" << (loweredFinish != nullptr)
                      << " user=" << (loweredUserCollect != nullptr)
                      << '\n';
        return fail("materialized collect did not lower through affine builder ownership state");
    }
    builderBorrow->isMutable = false;
    if (cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("CFG verifier accepted a forged shared collect builder borrow");
    builderBorrow->isMutable = true;
    if (!cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("restored materialized collect CFG did not verify");
    auto savedFinishTransfer = std::move(loweredFinish->args[0]);
    auto copiedBuilder = std::make_unique<moon::IdentifierExpr>();
    copiedBuilder->name = finishedBuilder->name;
    copiedBuilder->local = finishedBuilder->local;
    copiedBuilder->type = finishedBuilder->type;
    loweredFinish->args[0] = std::move(copiedBuilder);
    if (cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("CFG verifier accepted a copied collect builder at finish");
    loweredFinish->args[0] = std::move(savedFinishTransfer);
    if (!cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("restored collect builder transfer did not verify");
    const auto savedPushResultType = loweredPush->type;
    loweredPush->type = protocolI32Id;
    if (cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("CFG verifier accepted a forged collect push signature");
    loweredPush->type = savedPushResultType;
    if (!cfgVerifier.verify(*collectCfg, protocolModule))
        return fail("restored collect push signature did not verify");

    auto directCollectStructured = std::make_unique<moon::BlockStmt>();
    auto directCollectBinding = std::make_unique<moon::LetStmt>();
    directCollectBinding->name = "directCollected";
    directCollectBinding->type = collectedId;
    directCollectBinding->usage = luna::ownership::Usage::Affine;
    auto directCollect = makeDirectRangeTerminal(
        IteratorOp::Collect, collectedId, collectedId, true);
    directCollect->returnUsage = luna::ownership::Usage::Affine;
    directCollect->iteratorCollectTargetType = collectedId;
    directCollect->iteratorCollectBuilderType = iteratorStateId;
    directCollect->iteratorCollectBegin = {
        sealedCollectBegin->symbolId, sealedCollectBegin->contractId};
    directCollect->iteratorCollectPush = {
        sealedCollectPush->symbolId, sealedCollectPush->contractId};
    directCollect->iteratorCollectFinish = {
        sealedCollectFinish->symbolId, sealedCollectFinish->contractId};
    directCollectBinding->initializer = std::move(directCollect);
    directCollectStructured->stmts.push_back(
        std::move(directCollectBinding));
    auto directCollectCleanup = std::make_unique<moon::FreeStmt>();
    directCollectCleanup->isImplicit = true;
    directCollectCleanup->action = cleanupActionForType(collected);
    auto directCollectedIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    directCollectedIdentifier->name = "directCollected";
    directCollectedIdentifier->type = collectedId;
    directCollectCleanup->operand = std::move(
        directCollectedIdentifier);
    directCollectStructured->stmts.push_back(
        std::move(directCollectCleanup));
    auto directCollectCfg = cfgBuilder.build(
        std::move(directCollectStructured), {},
        moon::RegionKind::Function, protocolModule);
    bool directCollectCursor = false;
    bool directCollectBuilder = false;
    bool directCollectResult = false;
    bool directCollectRetainedIterator = false;
    if (directCollectCfg)
        for (const auto& local : directCollectCfg->locals) {
            directCollectCursor = directCollectCursor ||
                local.name.rfind("$terminal.cursor.", 0) == 0;
            directCollectBuilder = directCollectBuilder ||
                local.name.rfind(
                    "$terminal.collect.builder.", 0) == 0;
            directCollectResult = directCollectResult ||
                local.name.rfind(
                    "$terminal.collect.result.", 0) == 0;
            const auto* type = protocolModule.findType(local.type);
            directCollectRetainedIterator =
                directCollectRetainedIterator ||
                (type && type->kind == TypeKind::Iterator);
        }
    if (!directCollectCfg ||
        !cfgVerifier.verify(*directCollectCfg, protocolModule) ||
        !directCollectCursor || !directCollectBuilder ||
        !directCollectResult || directCollectRetainedIterator ||
        directCollectCfg->cleanups.size() != 3)
        return fail("direct collect did not erase to affine builder CFG state");

    const auto* shortIteratorType = module.findType(shortId);
    const auto* sharedIteratorType = module.findType(sharedId);
    if (!shortIteratorType || shortIteratorType->typeArgumentIds.empty() ||
        !sharedIteratorType || sharedIteratorType->innerTypeId.empty())
        return fail("array iterator fixtures lost their frozen type witnesses");
    const auto arrayId = shortIteratorType->typeArgumentIds.front();
    moon::Param arrayParameter;
    arrayParameter.name = "values";
    arrayParameter.type = arrayId;

    auto snapshotStructured = std::make_unique<moon::BlockStmt>();
    auto snapshotBinding = std::make_unique<moon::LetStmt>();
    snapshotBinding->name = "snapshot";
    snapshotBinding->type = shortId;
    snapshotBinding->usage = luna::ownership::Usage::Affine;
    snapshotBinding->materializesIteratorRecipe = true;
    auto into = std::make_unique<moon::CallExpr>();
    into->iteratorOp = IteratorOp::IntoIter;
    into->iteratorInputType = i32Id;
    into->iteratorOutputType = i32Id;
    into->type = shortId;
    auto intoMember = std::make_unique<moon::FieldAccessExpr>();
    intoMember->field = "into_iter";
    auto snapshotSource = std::make_unique<moon::IdentifierExpr>();
    snapshotSource->name = "values";
    snapshotSource->type = arrayId;
    intoMember->object = std::move(snapshotSource);
    into->callee = std::move(intoMember);
    snapshotBinding->initializer = std::move(into);
    snapshotStructured->stmts.push_back(std::move(snapshotBinding));
    auto snapshotLoop = std::make_unique<moon::ForStmt>();
    snapshotLoop->varName = "snapshotValue";
    snapshotLoop->elementType = i32Id;
    snapshotLoop->bindingUsage = luna::ownership::Usage::Copy;
    auto snapshotRecipe = std::make_unique<moon::IdentifierExpr>();
    snapshotRecipe->name = "snapshot";
    snapshotRecipe->type = shortId;
    snapshotLoop->iterable = std::move(snapshotRecipe);
    snapshotLoop->body = std::make_unique<moon::BlockStmt>();
    snapshotStructured->stmts.push_back(std::move(snapshotLoop));
    auto snapshotCfg = cfgBuilder.build(
        std::move(snapshotStructured), {arrayParameter},
        moon::RegionKind::Function, module);
    auto* snapshotSourceBinding = snapshotCfg &&
            !snapshotCfg->blocks.empty() &&
            !snapshotCfg->blocks[0].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              snapshotCfg->blocks[0].operations[0].get())
        : nullptr;
    auto* snapshotSourceRead = snapshotSourceBinding
        ? dynamic_cast<moon::IdentifierExpr*>(
              snapshotSourceBinding->initializer.get())
        : nullptr;
    if (!snapshotCfg || !cfgVerifier.verify(*snapshotCfg, module) ||
        snapshotCfg->locals.size() != 7 || !snapshotSourceBinding ||
        snapshotSourceBinding->type != arrayId || !snapshotSourceRead ||
        snapshotSourceRead->local != moon::LocalId{0})
        return fail("materialized Copy array did not snapshot its source at binding");

    auto sharedStructured = std::make_unique<moon::BlockStmt>();
    auto sharedLoop = std::make_unique<moon::ForStmt>();
    sharedLoop->varName = "element";
    sharedLoop->bindingUsage = luna::ownership::Usage::Copy;
    sharedLoop->elementType = sharedIteratorType->innerTypeId;
    auto iterCall = std::make_unique<moon::CallExpr>();
    iterCall->iteratorOp = IteratorOp::Iter;
    iterCall->iteratorInputType = i32Id;
    iterCall->iteratorOutputType = sharedIteratorType->innerTypeId;
    iterCall->type = sharedId;
    auto iterMember = std::make_unique<moon::FieldAccessExpr>();
    iterMember->field = "iter";
    auto arrayIdentifier = std::make_unique<moon::IdentifierExpr>();
    arrayIdentifier->name = "values";
    arrayIdentifier->type = arrayId;
    iterMember->object = std::move(arrayIdentifier);
    iterCall->callee = std::move(iterMember);
    sharedLoop->iterable = std::move(iterCall);
    sharedLoop->body = std::make_unique<moon::BlockStmt>();
    sharedStructured->stmts.push_back(std::move(sharedLoop));
    auto sharedCfg = cfgBuilder.build(
        std::move(sharedStructured), {arrayParameter},
        moon::RegionKind::Function, module);
    auto* sharedItem = sharedCfg && sharedCfg->blocks.size() > 3 &&
            !sharedCfg->blocks[3].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              sharedCfg->blocks[3].operations.front().get())
        : nullptr;
    if (!sharedCfg || !cfgVerifier.verify(*sharedCfg, module) ||
        sharedCfg->blocks.size() != 6 || sharedCfg->locals.size() != 4 ||
        !sharedItem ||
        !dynamic_cast<moon::BorrowExpr*>(sharedItem->initializer.get()))
        return fail("shared array recipe lost its canonical indexed borrow");

    const auto* sharedSliceType = module.findType(sharedSliceId);
    const auto* mutableSliceType = module.findType(mutableSliceId);
    if (!sharedSliceType || sharedSliceType->innerTypeId.empty() ||
        !mutableSliceType || mutableSliceType->innerTypeId.empty())
        return fail("slice iterator fixtures lost their frozen item types");
    auto sliceStructured = std::make_unique<moon::BlockStmt>();
    auto sliceLoop = std::make_unique<moon::ForStmt>();
    sliceLoop->varName = "sliceElement";
    sliceLoop->bindingUsage = luna::ownership::Usage::Copy;
    sliceLoop->elementType = sharedSliceType->innerTypeId;
    auto sliceIdentifier = std::make_unique<moon::IdentifierExpr>();
    sliceIdentifier->name = "view";
    sliceIdentifier->type = sliceId;
    sliceLoop->iterable = std::move(sliceIdentifier);
    sliceLoop->body = std::make_unique<moon::BlockStmt>();
    sliceStructured->stmts.push_back(std::move(sliceLoop));
    moon::Param sliceParameter;
    sliceParameter.name = "view";
    sliceParameter.type = sliceId;
    sliceParameter.relation = luna::ownership::Relation::SharedBorrow;
    auto sliceCfg = cfgBuilder.build(
        std::move(sliceStructured), {sliceParameter},
        moon::RegionKind::Function, module);
    auto* sliceLimitBinding = sliceCfg && sliceCfg->blocks.size() > 1 &&
            sliceCfg->blocks[1].operations.size() > 1
        ? dynamic_cast<moon::LetStmt*>(
              sliceCfg->blocks[1].operations[1].get())
        : nullptr;
    auto* sliceLength = sliceLimitBinding
        ? dynamic_cast<moon::SliceLengthExpr*>(
              sliceLimitBinding->initializer.get())
        : nullptr;
    auto* sliceItem = sliceCfg && sliceCfg->blocks.size() > 3 &&
            !sliceCfg->blocks[3].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              sliceCfg->blocks[3].operations.front().get())
        : nullptr;
    auto* sliceBorrow = sliceItem
        ? dynamic_cast<moon::BorrowExpr*>(sliceItem->initializer.get())
        : nullptr;
    auto* sliceIndex = sliceBorrow
        ? dynamic_cast<moon::IndexExpr*>(sliceBorrow->operand.get())
        : nullptr;
    auto* sliceLengthSource = sliceLength
        ? dynamic_cast<moon::IdentifierExpr*>(sliceLength->slice.get())
        : nullptr;
    auto* sliceIndexSource = sliceIndex
        ? dynamic_cast<moon::IdentifierExpr*>(sliceIndex->object.get())
        : nullptr;
    if (!sliceCfg || !cfgVerifier.verify(*sliceCfg, module) ||
        sliceCfg->blocks.size() != 6 || sliceCfg->locals.size() != 4 ||
        sliceCfg->blocks[1].operations.size() != 2 || !sliceLength ||
        !sliceLengthSource || sliceLengthSource->local != moon::LocalId{0} ||
        !sliceBorrow || !sliceIndex || !sliceIndexSource ||
        sliceIndexSource->local != moon::LocalId{0})
        return fail("shared slice recipe lost its canonical length/index projections");

    sliceLength->type = boolId;
    if (cfgVerifier.verify(*sliceCfg, module))
        return fail("CFG verifier accepted a non-usize slice length projection");
    sliceLength->type = usizeId;
    auto savedSliceOperand = std::move(sliceLength->slice);
    auto invalidSliceOperand = std::make_unique<moon::IntLiteralExpr>();
    invalidSliceOperand->value = 1;
    invalidSliceOperand->type = i32Id;
    sliceLength->slice = std::move(invalidSliceOperand);
    if (cfgVerifier.verify(*sliceCfg, module))
        return fail("CFG verifier accepted a non-slice length operand");
    sliceLength->slice = std::move(savedSliceOperand);
    if (!cfgVerifier.verify(*sliceCfg, module))
        return fail("restored canonical slice recipe did not verify");

    auto mutableSliceStructured = std::make_unique<moon::BlockStmt>();
    auto mutableSliceLoop = std::make_unique<moon::ForStmt>();
    mutableSliceLoop->varName = "mutableElement";
    mutableSliceLoop->bindingUsage = luna::ownership::Usage::Copy;
    mutableSliceLoop->elementType = mutableSliceType->innerTypeId;
    auto mutableIter = std::make_unique<moon::CallExpr>();
    mutableIter->iteratorOp = IteratorOp::IterMut;
    mutableIter->iteratorInputType = i32Id;
    mutableIter->iteratorOutputType = mutableSliceType->innerTypeId;
    mutableIter->type = mutableSliceId;
    auto mutableMember = std::make_unique<moon::FieldAccessExpr>();
    mutableMember->field = "iter_mut";
    auto mutableSource = std::make_unique<moon::IdentifierExpr>();
    mutableSource->name = "view";
    mutableSource->type = sliceId;
    mutableMember->object = std::move(mutableSource);
    mutableIter->callee = std::move(mutableMember);
    mutableSliceLoop->iterable = std::move(mutableIter);
    mutableSliceLoop->body = std::make_unique<moon::BlockStmt>();
    mutableSliceStructured->stmts.push_back(std::move(mutableSliceLoop));
    if (cfgBuilder.build(
            std::move(mutableSliceStructured), {sliceParameter},
            moon::RegionKind::Function, module))
        return fail("CFG builder accepted mutable iteration over a read-only slice");
    bool diagnosedMutableSlice = false;
    for (const auto& message : cfgBuilder.errors())
        if (message.find("require shared iteration") != std::string::npos)
            diagnosedMutableSlice = true;
    if (!diagnosedMutableSlice)
        return fail("CFG builder did not preserve the read-only slice boundary");

    auto lambdaStructured = std::make_unique<moon::BlockStmt>();
    auto lambdaStatement = std::make_unique<moon::ExprStmt>();
    auto lambda = std::make_unique<moon::LambdaExpr>();
    lambda->type = lambdaTypeId;
    lambda->closureType = lambdaTypeId;
    lambda->returnType = i32Id;
    moon::Param lambdaParameter;
    lambdaParameter.name = "input";
    lambdaParameter.type = i32Id;
    lambda->params.push_back(lambdaParameter);
    lambda->body = std::make_unique<moon::BlockStmt>();
    auto lambdaReturn = std::make_unique<moon::ReturnStmt>();
    auto lambdaResult = std::make_unique<moon::IdentifierExpr>();
    lambdaResult->name = "input";
    lambdaResult->type = i32Id;
    lambdaReturn->value = std::move(lambdaResult);
    lambda->body->stmts.push_back(std::move(lambdaReturn));
    auto* canonicalLambda = lambda.get();
    lambdaStatement->expr = std::move(lambda);
    lambdaStructured->stmts.push_back(std::move(lambdaStatement));
    auto lambdaCfg = cfgBuilder.build(
        std::move(lambdaStructured), {},
        moon::RegionKind::Function, module);
    const auto* lambdaRoot = lambdaCfg && canonicalLambda->controlFlow
        ? canonicalLambda->controlFlow->findRegion(
              canonicalLambda->controlFlow->rootRegion)
        : nullptr;
    if (!lambdaCfg || !cfgVerifier.verify(*lambdaCfg, module) ||
        canonicalLambda->body || !canonicalLambda->controlFlow ||
        !lambdaRoot || lambdaRoot->kind != moon::RegionKind::Lambda ||
        canonicalLambda->controlFlow->locals.size() != 1 ||
        canonicalLambda->controlFlow->locals.front().kind !=
            moon::LocalKind::Parameter)
        return fail("lambda body did not become an independent canonical CFG");
    auto* lambdaReturnId = canonicalLambda->controlFlow->blocks.empty()
        ? nullptr
        : dynamic_cast<moon::IdentifierExpr*>(
              canonicalLambda->controlFlow->blocks.front()
                  .terminator.operand.get());
    if (!lambdaReturnId || lambdaReturnId->local != moon::LocalId{0})
        return fail("lambda canonical CFG did not bind its parameter LocalId");
    lambdaReturnId->local = moon::LocalId{99};
    if (cfgVerifier.verify(*lambdaCfg, module))
        return fail("parent CFG verifier accepted an invalid nested lambda CFG");
    lambdaReturnId->local = moon::LocalId{0};
    canonicalLambda->body = std::make_unique<moon::BlockStmt>();
    if (cfgVerifier.verify(*lambdaCfg, module))
        return fail("CFG verifier accepted dual lambda execution bodies");
    canonicalLambda->body.reset();
    canonicalLambda->captures.push_back("outer");
    if (cfgVerifier.verify(*lambdaCfg, module))
        return fail("CFG verifier accepted a lambda without a capture layout");
    canonicalLambda->captures.clear();
    canonicalLambda->controlFlow->regions[
        canonicalLambda->controlFlow->rootRegion.value].kind =
            moon::RegionKind::Function;
    if (cfgVerifier.verify(*lambdaCfg, module))
        return fail("CFG verifier accepted a non-lambda nested root region");
    canonicalLambda->controlFlow->regions[
        canonicalLambda->controlFlow->rootRegion.value].kind =
            moon::RegionKind::Lambda;
    if (!cfgVerifier.verify(*lambdaCfg, module))
        return fail("restored canonical lambda CFG did not verify");

    auto capturedStructured = std::make_unique<moon::BlockStmt>();
    auto offsetBinding = std::make_unique<moon::LetStmt>();
    offsetBinding->name = "offset";
    offsetBinding->type = i32Id;
    offsetBinding->usage = luna::ownership::Usage::Copy;
    auto offsetLiteral = std::make_unique<moon::IntLiteralExpr>();
    offsetLiteral->value = 41;
    offsetLiteral->type = i32Id;
    offsetBinding->initializer = std::move(offsetLiteral);
    capturedStructured->stmts.push_back(std::move(offsetBinding));
    auto capturedStatement = std::make_unique<moon::ExprStmt>();
    auto capturedMake = std::make_unique<moon::MakeClosureExpr>();
    capturedMake->type = closureTypeId;
    auto capturedLambda = std::make_unique<moon::LambdaExpr>();
    capturedLambda->type = closureTypeId;
    capturedLambda->closureType = closureTypeId;
    capturedLambda->returnType = i32Id;
    capturedLambda->captures.push_back("offset");
    capturedLambda->envParamName = "$closure.env";
    moon::Param capturedParameter;
    capturedParameter.name = "input";
    capturedParameter.type = i32Id;
    capturedLambda->params.push_back(capturedParameter);
    capturedLambda->body = std::make_unique<moon::BlockStmt>();
    auto capturedReturn = std::make_unique<moon::ReturnStmt>();
    auto capturedAdd = std::make_unique<moon::BinaryExpr>();
    capturedAdd->op = moon::Operator::Add;
    auto capturedOffset = std::make_unique<moon::IdentifierExpr>();
    capturedOffset->name = "offset";
    capturedOffset->type = i32Id;
    auto capturedInput = std::make_unique<moon::IdentifierExpr>();
    capturedInput->name = "input";
    capturedInput->type = i32Id;
    capturedAdd->lhs = std::move(capturedOffset);
    capturedAdd->rhs = std::move(capturedInput);
    capturedReturn->value = std::move(capturedAdd);
    capturedLambda->body->stmts.push_back(std::move(capturedReturn));
    auto* capturedCanonicalLambda = capturedLambda.get();
    capturedMake->lambda = std::move(capturedLambda);
    auto capturedValueReference = std::make_unique<moon::IdentifierExpr>();
    capturedValueReference->name = "offset";
    capturedValueReference->type = i32Id;
    capturedMake->capturedValues.push_back(
        std::move(capturedValueReference));
    capturedStatement->expr = std::move(capturedMake);
    capturedStructured->stmts.push_back(std::move(capturedStatement));
    auto capturedCfg = cfgBuilder.build(
        std::move(capturedStructured), {},
        moon::RegionKind::Function, module);
    const auto* capturedRoot =
        capturedCfg && capturedCanonicalLambda->controlFlow
            ? capturedCanonicalLambda->controlFlow->findRegion(
                  capturedCanonicalLambda->controlFlow->rootRegion)
            : nullptr;
    size_t capturedParameterCount = 0;
    bool capturedEnvParameter = false;
    if (capturedCfg && capturedCanonicalLambda->controlFlow)
        for (const auto& local : capturedCanonicalLambda->controlFlow->locals)
            if (local.kind == moon::LocalKind::Parameter) {
                ++capturedParameterCount;
                if (local.name == "$closure.env")
                    capturedEnvParameter = true;
            }
    if (!capturedCfg || !cfgVerifier.verify(*capturedCfg, module) ||
        capturedCanonicalLambda->body ||
        !capturedCanonicalLambda->controlFlow ||
        !capturedRoot ||
        capturedRoot->kind != moon::RegionKind::Lambda ||
        capturedParameterCount != 2 || !capturedEnvParameter)
        return fail("capturing lambda did not become a verified canonical CFG with an environment parameter");
    bool capturedEnvLoad = false;
    for (const auto& block : capturedCanonicalLambda->controlFlow->blocks)
        if (auto* returned = dynamic_cast<moon::BinaryExpr*>(
                block.terminator.operand.get()))
            if (dynamic_cast<moon::EnvLoadExpr*>(
                    returned->lhs.get())) {
                const auto* envLoad = dynamic_cast<moon::EnvLoadExpr*>(
                    returned->lhs.get());
                capturedEnvLoad = envLoad &&
                    !envLoad->envLocal.empty() &&
                    envLoad->fieldIndex == 0;
            }
    if (!capturedEnvLoad)
        return fail("capturing lambda did not rewrite its capture read into an environment load");
    if (capturedCanonicalLambda->captures.empty() ||
        capturedCanonicalLambda->envParamName.empty())
        return fail("capturing lambda lost its capture record or environment parameter name");

    const auto makeRecipeLambda = [&](moon::TypeRef closureType,
                                      moon::TypeRef returnType,
                                      bool predicate) {
        auto value = std::make_unique<moon::LambdaExpr>();
        value->type = closureType;
        value->closureType = closureType;
        value->returnType = returnType;
        moon::Param parameter;
        parameter.name = "input";
        parameter.type = i32Id;
        value->params.push_back(parameter);
        value->body = std::make_unique<moon::BlockStmt>();
        auto returned = std::make_unique<moon::ReturnStmt>();
        if (predicate) {
            auto accepted = std::make_unique<moon::BoolLiteralExpr>();
            accepted->value = true;
            accepted->type = boolId;
            returned->value = std::move(accepted);
        } else {
            auto input = std::make_unique<moon::IdentifierExpr>();
            input->name = "input";
            input->type = i32Id;
            returned->value = std::move(input);
        }
        value->body->stmts.push_back(std::move(returned));
        return value;
    };

    auto pipelineStructured = std::make_unique<moon::BlockStmt>();
    auto pipelineLoop = std::make_unique<moon::ForStmt>();
    pipelineLoop->varName = "mapped";
    pipelineLoop->elementType = i32Id;
    pipelineLoop->bindingUsage = luna::ownership::Usage::Copy;
    auto pipelineRange = std::make_unique<moon::CallExpr>();
    pipelineRange->iteratorOp = IteratorOp::Range;
    pipelineRange->iteratorInputType = i32Id;
    pipelineRange->iteratorOutputType = i32Id;
    pipelineRange->type = rangeId;
    auto pipelineRangeCallee = std::make_unique<moon::IdentifierExpr>();
    pipelineRangeCallee->name = "range";
    pipelineRange->callee = std::move(pipelineRangeCallee);
    auto pipelineStart = std::make_unique<moon::IntLiteralExpr>();
    pipelineStart->value = 0;
    pipelineStart->type = i32Id;
    auto pipelineEnd = std::make_unique<moon::IntLiteralExpr>();
    pipelineEnd->value = 4;
    pipelineEnd->type = i32Id;
    pipelineRange->args.push_back(std::move(pipelineStart));
    pipelineRange->args.push_back(std::move(pipelineEnd));

    auto pipelineMap = std::make_unique<moon::CallExpr>();
    pipelineMap->iteratorOp = IteratorOp::Map;
    pipelineMap->iteratorInputType = i32Id;
    pipelineMap->iteratorOutputType = i32Id;
    pipelineMap->type = rangeId;
    auto pipelineMapMember = std::make_unique<moon::FieldAccessExpr>();
    pipelineMapMember->field = "map";
    pipelineMapMember->object = std::move(pipelineRange);
    pipelineMap->callee = std::move(pipelineMapMember);
    pipelineMap->args.push_back(makeRecipeLambda(
        lambdaTypeId, i32Id, false));

    auto pipelineFilter = std::make_unique<moon::CallExpr>();
    pipelineFilter->iteratorOp = IteratorOp::Filter;
    pipelineFilter->iteratorInputType = i32Id;
    pipelineFilter->iteratorOutputType = i32Id;
    pipelineFilter->type = rangeId;
    auto pipelineFilterMember = std::make_unique<moon::FieldAccessExpr>();
    pipelineFilterMember->field = "filter";
    pipelineFilterMember->object = std::move(pipelineMap);
    pipelineFilter->callee = std::move(pipelineFilterMember);
    pipelineFilter->args.push_back(makeRecipeLambda(
        predicateTypeId, boolId, true));

    auto pipelineTake = std::make_unique<moon::CallExpr>();
    pipelineTake->iteratorOp = IteratorOp::Take;
    pipelineTake->iteratorInputType = i32Id;
    pipelineTake->iteratorOutputType = i32Id;
    pipelineTake->type = rangeId;
    auto pipelineTakeMember = std::make_unique<moon::FieldAccessExpr>();
    pipelineTakeMember->field = "take";
    pipelineTakeMember->object = std::move(pipelineFilter);
    pipelineTake->callee = std::move(pipelineTakeMember);
    auto pipelineCount = std::make_unique<moon::IntLiteralExpr>();
    pipelineCount->value = 2;
    pipelineCount->type = i32Id;
    pipelineTake->args.push_back(std::move(pipelineCount));
    pipelineLoop->iterable = std::move(pipelineTake);
    pipelineLoop->body = std::make_unique<moon::BlockStmt>();
    auto pipelineUse = std::make_unique<moon::ExprStmt>();
    auto pipelineItem = std::make_unique<moon::IdentifierExpr>();
    pipelineItem->name = "mapped";
    pipelineItem->type = i32Id;
    pipelineUse->expr = std::move(pipelineItem);
    pipelineLoop->body->stmts.push_back(std::move(pipelineUse));
    pipelineStructured->stmts.push_back(std::move(pipelineLoop));

    auto pipelineCfg = cfgBuilder.build(
        std::move(pipelineStructured), {},
        moon::RegionKind::Function, module);
    moon::CallExpr* mapInvocation = nullptr;
    moon::CallExpr* filterInvocation = nullptr;
    size_t canonicalAdapterLambdas = 0;
    if (pipelineCfg) {
        for (auto& block : pipelineCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get());
                if (!declaration) continue;
                if (auto* adapter = dynamic_cast<moon::LambdaExpr*>(
                        declaration->initializer.get())) {
                    if (!adapter->body && adapter->controlFlow)
                        ++canonicalAdapterLambdas;
                } else if (auto* invocation = dynamic_cast<moon::CallExpr*>(
                               declaration->initializer.get())) {
                    if (invocation->type == i32Id)
                        mapInvocation = invocation;
                }
            }
            if (auto* invocation = dynamic_cast<moon::CallExpr*>(
                    block.terminator.operand.get());
                invocation && invocation->type == boolId)
                filterInvocation = invocation;
        }
    }
    if (!pipelineCfg || !cfgVerifier.verify(*pipelineCfg, module) ||
        pipelineCfg->blocks.size() != 11 ||
        pipelineCfg->locals.size() != 8 ||
        canonicalAdapterLambdas != 2 || !mapInvocation ||
        !filterInvocation ||
        mapInvocation->iteratorOp != IteratorOp::None ||
        filterInvocation->iteratorOp != IteratorOp::None)
        return fail("map/filter/take recipe did not expand to ordinary canonical CFG");
    mapInvocation->type = boolId;
    if (cfgVerifier.verify(*pipelineCfg, module))
        return fail("CFG verifier accepted a forged local closure call signature");
    mapInvocation->type = i32Id;
    mapInvocation->returnUsage = luna::ownership::Usage::Affine;
    if (cfgVerifier.verify(*pipelineCfg, module))
        return fail("CFG verifier accepted a forged local closure result contract");
    mapInvocation->returnUsage = luna::ownership::Usage::Copy;
    if (!cfgVerifier.verify(*pipelineCfg, module))
        return fail("restored map/filter canonical CFG did not verify");

    auto moveMapStructured = std::make_unique<moon::BlockStmt>();
    auto moveMapLoop = std::make_unique<moon::ForStmt>();
    moveMapLoop->varName = "owned";
    moveMapLoop->elementType = stringId;
    moveMapLoop->bindingUsage = luna::ownership::Usage::Affine;
    auto moveMapRange = std::make_unique<moon::CallExpr>();
    moveMapRange->iteratorOp = IteratorOp::Range;
    moveMapRange->iteratorInputType = i32Id;
    moveMapRange->iteratorOutputType = i32Id;
    moveMapRange->type = rangeId;
    auto moveMapRangeCallee = std::make_unique<moon::IdentifierExpr>();
    moveMapRangeCallee->name = "range";
    moveMapRange->callee = std::move(moveMapRangeCallee);
    auto moveMapStart = std::make_unique<moon::IntLiteralExpr>();
    moveMapStart->value = 0;
    moveMapStart->type = i32Id;
    auto moveMapEnd = std::make_unique<moon::IntLiteralExpr>();
    moveMapEnd->value = 1;
    moveMapEnd->type = i32Id;
    moveMapRange->args.push_back(std::move(moveMapStart));
    moveMapRange->args.push_back(std::move(moveMapEnd));
    auto moveMapCall = std::make_unique<moon::CallExpr>();
    moveMapCall->iteratorOp = IteratorOp::Map;
    moveMapCall->iteratorInputType = i32Id;
    moveMapCall->iteratorOutputType = stringId;
    moveMapCall->type = moveMapIteratorId;
    auto moveMapMember = std::make_unique<moon::FieldAccessExpr>();
    moveMapMember->field = "map";
    moveMapMember->object = std::move(moveMapRange);
    moveMapCall->callee = std::move(moveMapMember);
    auto moveMapLambda = std::make_unique<moon::LambdaExpr>();
    moveMapLambda->type = moveMapTypeId;
    moveMapLambda->closureType = moveMapTypeId;
    moveMapLambda->returnType = stringId;
    moon::Param moveMapParameter;
    moveMapParameter.name = "input";
    moveMapParameter.type = i32Id;
    moveMapLambda->params.push_back(moveMapParameter);
    moveMapLambda->body = std::make_unique<moon::BlockStmt>();
    auto moveMapReturn = std::make_unique<moon::ReturnStmt>();
    auto moveMapValue = std::make_unique<moon::StringLiteralExpr>();
    moveMapValue->value = "owned";
    moveMapValue->type = stringId;
    moveMapReturn->value = std::move(moveMapValue);
    moveMapLambda->body->stmts.push_back(std::move(moveMapReturn));
    moveMapCall->args.push_back(std::move(moveMapLambda));

    auto moveFilterCall = std::make_unique<moon::CallExpr>();
    moveFilterCall->iteratorOp = IteratorOp::Filter;
    moveFilterCall->iteratorInputType = stringId;
    moveFilterCall->iteratorOutputType = stringId;
    moveFilterCall->type = moveMapIteratorId;
    auto moveFilterMember = std::make_unique<moon::FieldAccessExpr>();
    moveFilterMember->field = "filter";
    moveFilterMember->object = std::move(moveMapCall);
    moveFilterCall->callee = std::move(moveFilterMember);
    auto moveFilterLambda = std::make_unique<moon::LambdaExpr>();
    moveFilterLambda->type = affinePredicateTypeId;
    moveFilterLambda->closureType = affinePredicateTypeId;
    moveFilterLambda->returnType = boolId;
    moon::Param moveFilterParameter;
    moveFilterParameter.name = "value";
    moveFilterParameter.type = stringId;
    moveFilterParameter.relation =
        luna::ownership::Relation::SharedBorrow;
    moveFilterParameter.usage = luna::ownership::Usage::Copy;
    moveFilterLambda->params.push_back(moveFilterParameter);
    moveFilterLambda->body = std::make_unique<moon::BlockStmt>();
    auto moveFilterReturn = std::make_unique<moon::ReturnStmt>();
    auto moveFilterAccepted = std::make_unique<moon::BoolLiteralExpr>();
    moveFilterAccepted->value = true;
    moveFilterAccepted->type = boolId;
    moveFilterReturn->value = std::move(moveFilterAccepted);
    moveFilterLambda->body->stmts.push_back(
        std::move(moveFilterReturn));
    moveFilterCall->args.push_back(std::move(moveFilterLambda));
    auto moveTakeCall = std::make_unique<moon::CallExpr>();
    moveTakeCall->iteratorOp = IteratorOp::Take;
    moveTakeCall->iteratorInputType = stringId;
    moveTakeCall->iteratorOutputType = stringId;
    moveTakeCall->type = moveMapIteratorId;
    auto moveTakeMember = std::make_unique<moon::FieldAccessExpr>();
    moveTakeMember->field = "take";
    moveTakeMember->object = std::move(moveFilterCall);
    moveTakeCall->callee = std::move(moveTakeMember);
    auto moveTakeCount = std::make_unique<moon::IntLiteralExpr>();
    moveTakeCount->value = 1;
    moveTakeCount->type = i32Id;
    moveTakeCall->args.push_back(std::move(moveTakeCount));
    auto moveOwnedMapCall = std::make_unique<moon::CallExpr>();
    moveOwnedMapCall->iteratorOp = IteratorOp::Map;
    moveOwnedMapCall->iteratorInputType = stringId;
    moveOwnedMapCall->iteratorOutputType = stringId;
    moveOwnedMapCall->type = moveMapIteratorId;
    auto moveOwnedMapMember = std::make_unique<moon::FieldAccessExpr>();
    moveOwnedMapMember->field = "map";
    moveOwnedMapMember->object = std::move(moveTakeCall);
    moveOwnedMapCall->callee = std::move(moveOwnedMapMember);
    auto moveOwnedMapLambda = std::make_unique<moon::LambdaExpr>();
    moveOwnedMapLambda->type = affineIdentityTypeId;
    moveOwnedMapLambda->closureType = affineIdentityTypeId;
    moveOwnedMapLambda->returnType = stringId;
    moon::Param moveOwnedMapParameter;
    moveOwnedMapParameter.name = "value";
    moveOwnedMapParameter.type = stringId;
    moveOwnedMapParameter.relation = luna::ownership::Relation::Owned;
    moveOwnedMapParameter.usage = luna::ownership::Usage::Affine;
    moveOwnedMapLambda->params.push_back(moveOwnedMapParameter);
    moveOwnedMapLambda->body = std::make_unique<moon::BlockStmt>();
    auto moveOwnedMapReturn = std::make_unique<moon::ReturnStmt>();
    auto moveOwnedMapTransfer = std::make_unique<moon::MoveExpr>();
    moveOwnedMapTransfer->type = stringId;
    auto moveOwnedMapValue = std::make_unique<moon::IdentifierExpr>();
    moveOwnedMapValue->name = "value";
    moveOwnedMapValue->type = stringId;
    moveOwnedMapTransfer->operand = std::move(moveOwnedMapValue);
    moveOwnedMapReturn->value = std::move(moveOwnedMapTransfer);
    moveOwnedMapLambda->body->stmts.push_back(
        std::move(moveOwnedMapReturn));
    moveOwnedMapCall->args.push_back(std::move(moveOwnedMapLambda));
    moveMapLoop->iterable = std::move(moveOwnedMapCall);
    moveMapLoop->body = std::make_unique<moon::BlockStmt>();
    auto moveMapCleanup = std::make_unique<moon::FreeStmt>();
    moveMapCleanup->isImplicit = true;
    moveMapCleanup->action =
        luna::ownership::CleanupAction::Deallocate;
    auto moveMapCleanupTarget =
        std::make_unique<moon::IdentifierExpr>();
    moveMapCleanupTarget->name = "owned";
    moveMapCleanupTarget->type = stringId;
    moveMapCleanup->operand = std::move(moveMapCleanupTarget);
    moveMapLoop->body->stmts.push_back(std::move(moveMapCleanup));
    moveMapStructured->stmts.push_back(std::move(moveMapLoop));
    auto moveMapCfg = cfgBuilder.build(
        std::move(moveMapStructured), {},
        moon::RegionKind::Function, module);
    moon::LetStmt* moveMapBorrowedTemporary = nullptr;
    moon::LetStmt* moveMapFinalTemporary = nullptr;
    moon::LetStmt* moveMapBinding = nullptr;
    moon::CallExpr* moveOwnedMapInvocation = nullptr;
    moon::CallExpr* moveFilterInvocation = nullptr;
    moon::LambdaExpr* moveFilterCanonicalLambda = nullptr;
    moon::CleanupId moveMapBorrowedTemporaryCleanup;
    moon::CleanupId moveMapFinalTemporaryCleanup;
    moon::CleanupId moveMapBindingCleanup;
    if (moveMapCfg) {
        for (auto& block : moveMapCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration =
                    dynamic_cast<moon::LetStmt*>(operation.get());
                if (!declaration) continue;
                if (auto* lambda = dynamic_cast<moon::LambdaExpr*>(
                        declaration->initializer.get());
                    lambda &&
                    lambda->closureType == affinePredicateTypeId)
                    moveFilterCanonicalLambda = lambda;
                if (declaration->name == "owned")
                    moveMapBinding = declaration;
                else if (auto* invocation =
                             dynamic_cast<moon::CallExpr*>(
                                 declaration->initializer.get());
                         invocation && declaration->type == stringId) {
                    const auto* callee =
                        dynamic_cast<moon::IdentifierExpr*>(
                            invocation->callee.get());
                    if (callee && callee->type == moveMapTypeId)
                        moveMapBorrowedTemporary = declaration;
                    if (callee && callee->type == affineIdentityTypeId) {
                        moveMapFinalTemporary = declaration;
                        moveOwnedMapInvocation = invocation;
                    }
                }
            }
            auto* invocation = dynamic_cast<moon::CallExpr*>(
                block.terminator.operand.get());
            if (invocation && invocation->type == boolId)
                moveFilterInvocation = invocation;
        }
        for (const auto& cleanup : moveMapCfg->cleanups) {
            if (moveMapBorrowedTemporary &&
                cleanup.place.root == moveMapBorrowedTemporary->local)
                moveMapBorrowedTemporaryCleanup = cleanup.id;
            if (moveMapFinalTemporary &&
                cleanup.place.root == moveMapFinalTemporary->local)
                moveMapFinalTemporaryCleanup = cleanup.id;
            if (moveMapBinding &&
                cleanup.place.root == moveMapBinding->local)
                moveMapBindingCleanup = cleanup.id;
        }
    }
    auto* moveMapTransfer = moveMapBinding
        ? dynamic_cast<moon::MoveExpr*>(
              moveMapBinding->initializer.get())
        : nullptr;
    auto* moveMapTransferSource = moveMapTransfer
        ? dynamic_cast<moon::IdentifierExpr*>(
              moveMapTransfer->operand.get())
        : nullptr;
    auto* moveOwnedMapArgumentTransfer = moveOwnedMapInvocation &&
            moveOwnedMapInvocation->args.size() == 1
        ? dynamic_cast<moon::MoveExpr*>(
              moveOwnedMapInvocation->args.front().get())
        : nullptr;
    auto* moveOwnedMapTransferSource = moveOwnedMapArgumentTransfer
        ? dynamic_cast<moon::IdentifierExpr*>(
              moveOwnedMapArgumentTransfer->operand.get())
        : nullptr;
    auto* moveFilterBorrow = moveFilterInvocation &&
            moveFilterInvocation->args.size() == 1
        ? dynamic_cast<moon::BorrowExpr*>(
              moveFilterInvocation->args.front().get())
        : nullptr;
    auto* moveFilterBorrowSource = moveFilterBorrow
        ? dynamic_cast<moon::IdentifierExpr*>(
              moveFilterBorrow->operand.get())
        : nullptr;
    std::vector<moon::CleanupId>* moveMapCleanupEdge = nullptr;
    std::vector<moon::CleanupId>* moveFilterCleanupEdge = nullptr;
    std::vector<moon::CleanupId>* moveTakeCleanupEdge = nullptr;
    size_t borrowedTemporaryCleanupEdges = 0;
    size_t finalTemporaryCleanupEdges = 0;
    size_t bindingCleanupEdges = 0;
    if (moveMapCfg)
        for (auto& block : moveMapCfg->blocks) {
            const auto* branchCall = dynamic_cast<moon::CallExpr*>(
                block.terminator.operand.get());
            const auto* branchBinary = dynamic_cast<moon::BinaryExpr*>(
                block.terminator.operand.get());
            const auto inspectEdge = [&](
                std::vector<moon::CleanupId>& cleanups) {
                for (const auto cleanup : cleanups) {
                    if (cleanup == moveMapBorrowedTemporaryCleanup) {
                        ++borrowedTemporaryCleanupEdges;
                        if (branchCall && branchCall->type == boolId)
                            moveFilterCleanupEdge = &cleanups;
                        if (branchBinary &&
                            branchBinary->op == moon::Operator::Greater)
                            moveTakeCleanupEdge = &cleanups;
                    }
                    if (cleanup == moveMapFinalTemporaryCleanup)
                        ++finalTemporaryCleanupEdges;
                    if (cleanup == moveMapBindingCleanup) {
                        ++bindingCleanupEdges;
                        moveMapCleanupEdge = &cleanups;
                    }
                }
            };
            inspectEdge(block.terminator.primary.cleanups);
            inspectEdge(block.terminator.secondary.cleanups);
            for (auto& item : block.terminator.cases)
                inspectEdge(item.edge.cleanups);
            inspectEdge(block.terminator.exitCleanups);
        }
    const auto* moveMapBorrowedTemporaryLocal =
        moveMapCfg && moveMapBorrowedTemporary
        ? moveMapCfg->findLocal(moveMapBorrowedTemporary->local)
        : nullptr;
    const auto* moveMapFinalTemporaryLocal =
        moveMapCfg && moveMapFinalTemporary
        ? moveMapCfg->findLocal(moveMapFinalTemporary->local)
        : nullptr;
    auto* borrowedFilterParameter = moveFilterCanonicalLambda &&
            moveFilterCanonicalLambda->controlFlow &&
            !moveFilterCanonicalLambda->controlFlow->locals.empty()
        ? &moveFilterCanonicalLambda->controlFlow->locals.front()
        : nullptr;
    if (!moveMapCfg || !cfgVerifier.verify(*moveMapCfg, module) ||
        !moveMapBorrowedTemporaryLocal ||
        moveMapBorrowedTemporaryLocal->kind !=
            moon::LocalKind::Synthetic ||
        moveMapBorrowedTemporaryLocal->usage !=
            luna::ownership::Usage::Affine ||
        !moveMapFinalTemporaryLocal ||
        moveMapFinalTemporaryLocal->kind !=
            moon::LocalKind::Synthetic ||
        moveMapFinalTemporaryLocal->usage !=
            luna::ownership::Usage::Affine ||
        !moveMapTransferSource ||
        moveMapTransferSource->local != moveMapFinalTemporary->local ||
        !moveOwnedMapTransferSource ||
        moveOwnedMapTransferSource->local !=
            moveMapBorrowedTemporary->local ||
        !moveFilterBorrow || moveFilterBorrow->isMutable ||
        !moveFilterBorrowSource ||
        moveFilterBorrowSource->local !=
            moveMapBorrowedTemporary->local ||
        !borrowedFilterParameter ||
        borrowedFilterParameter->relation !=
            luna::ownership::Relation::SharedBorrow ||
        borrowedFilterParameter->usage !=
            luna::ownership::Usage::Copy ||
        !moveFilterCanonicalLambda->controlFlow->cleanups.empty() ||
        moveMapBorrowedTemporaryCleanup.empty() ||
        moveMapFinalTemporaryCleanup.empty() ||
        moveMapBindingCleanup.empty() ||
        borrowedTemporaryCleanupEdges != 2 ||
        finalTemporaryCleanupEdges != 0 || bindingCleanupEdges != 1 ||
        !moveFilterCleanupEdge || !moveTakeCleanupEdge ||
        !moveMapCleanupEdge) {
        for (const auto& error : cfgVerifier.errors())
            std::cerr << error << '\n';
        return fail(
            "affine map/filter item did not preserve per-iteration cleanup state");
    }

    borrowedFilterParameter->usage = luna::ownership::Usage::Affine;
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted affine cardinality on a borrowed local");
    borrowedFilterParameter->usage = luna::ownership::Usage::Copy;

    auto savedOwnedMapTransfer = std::move(
        moveOwnedMapInvocation->args.front());
    auto copiedOwnedMapInput = std::make_unique<moon::IdentifierExpr>();
    copiedOwnedMapInput->name = moveMapBorrowedTemporary->name;
    copiedOwnedMapInput->local = moveMapBorrowedTemporary->local;
    copiedOwnedMapInput->type = moveMapBorrowedTemporary->type;
    moveOwnedMapInvocation->args.front() = std::move(copiedOwnedMapInput);
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted a copied affine input to map");
    moveOwnedMapInvocation->args.front() = std::move(savedOwnedMapTransfer);

    auto savedMoveMapTransfer = std::move(moveMapBinding->initializer);
    auto copiedMoveMapResult = std::make_unique<moon::IdentifierExpr>();
    copiedMoveMapResult->name = moveMapFinalTemporary->name;
    copiedMoveMapResult->local = moveMapFinalTemporary->local;
    copiedMoveMapResult->type = moveMapFinalTemporary->type;
    moveMapBinding->initializer = std::move(copiedMoveMapResult);
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted a copied affine map result");
    moveMapBinding->initializer = std::move(savedMoveMapTransfer);

    auto savedMoveFilterCleanupEdge = *moveFilterCleanupEdge;
    moveFilterCleanupEdge->clear();
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted a missing affine filter rejection cleanup");
    *moveFilterCleanupEdge = std::move(savedMoveFilterCleanupEdge);

    auto savedMoveTakeCleanupEdge = *moveTakeCleanupEdge;
    moveTakeCleanupEdge->clear();
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted a missing affine take rejection cleanup");
    *moveTakeCleanupEdge = std::move(savedMoveTakeCleanupEdge);

    auto savedMoveMapCleanupEdge = *moveMapCleanupEdge;
    moveMapCleanupEdge->clear();
    if (cfgVerifier.verify(*moveMapCfg, module))
        return fail("CFG verifier accepted a missing affine map body cleanup");
    *moveMapCleanupEdge = std::move(savedMoveMapCleanupEdge);
    if (!cfgVerifier.verify(*moveMapCfg, module))
        return fail("restored affine final map CFG did not verify");

    // The sealed payload must not observe later mutations of the frontend
    // object from which it was frozen.
    product->fields.clear();
    moon::TypeMaterializer materializer(module);
    const auto restoredProduct = materializer.materialize(productId);
    if (!restoredProduct || restoredProduct.get() == product.get() ||
        restoredProduct->fields.size() != 1 ||
        restoredProduct->fields.front().name != "value")
        return fail("sealed MoonIR retained frontend Type object identity");
    const auto restoredForward = materializer.materialize(forwardId);
    if (!restoredForward || restoredForward->fields.size() != 1)
        return fail("completed nominal payload did not replace its forward placeholder");

    const auto restoredShort = materializer.materialize(shortId);
    const auto restoredLong = materializer.materialize(longId);
    if (!restoredShort || !restoredLong ||
        restoredShort->typeArgs.size() != 1 ||
        restoredLong->typeArgs.size() != 1 ||
        restoredShort->typeArgs.front()->arrayLength != 2 ||
        restoredLong->typeArgs.front()->arrayLength != 5)
        return fail("canonical type materialization lost iterator source shape");

    moon::Module reverse;
    reverse.name = module.name;
    reverse.registerType(longIterator);
    reverse.registerType(shortIterator);
    reverse.registerType(sharedIterator);
    reverse.registerType(mutableSliceIterator);
    reverse.registerType(slice);
    reverse.registerType(sharedSliceIterator);
    reverse.registerType(rangeIterator);
    reverse.registerType(Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot"));
    reverse.registerType(Type::makeStruct(
        "OwnedSnapshot", {{"owned", TyString}, {"value", TyI32}},
        "canonical.test::OwnedSnapshot"));
    reverse.registerType(TyUSize);
    reverse.registerType(TyString);
    reverse.registerType(guardedArray);
    reverse.registerType(TyBool);
    reverse.registerType(TyUnit);
    reverse.registerType(resultI32Bool);
    reverse.registerType(lambdaType);
    reverse.registerType(closureType);
    reverse.registerType(predicateType);
    reverse.registerType(reducerType);
    reverse.registerType(affineReducerType);
    reverse.registerType(linearReducerType);
    reverse.registerType(affineValueProducerType);
    reverse.registerType(actionType);
    reverse.registerType(unitConsumerType);
    reverse.registerType(unitOrderedConsumerType);
    reverse.registerType(moveMapType);
    reverse.registerType(affinePredicateType);
    reverse.registerType(affineIdentityType);
    reverse.registerType(moveMapIterator);
    reverse.registerType(inlineProduct);
    reverse.registerType(Type::makeEnum(
        "Choice", {{"None", {}}, {"Some", {TyI32}}},
        "canonical.test::Choice"));
    reverse.registerType(completedForward);
    reverse.registerType(forward);
    reverse.sealTypeTable();
    if (module.typeTable.size() != reverse.typeTable.size())
        return fail("type table depends on registration order");
    for (size_t index = 0; index < module.typeTable.size(); ++index) {
        if (module.typeTable[index].id != reverse.typeTable[index].id ||
            module.typeTable[index].canonicalType !=
                reverse.typeTable[index].canonicalType ||
            module.typeTable[index].canonicalShape !=
                reverse.typeTable[index].canonicalShape)
            return fail("sealed type table is not deterministic");
    }

    moon::Verifier verifier;
    if (!verifier.verify(module))
        return fail("canonical type-only module failed independent verification");
    if (!verifier.verify(reverse))
        return fail("reverse-order canonical module failed independent verification");

    const std::string sealableFunctionSource = R"luna(
package canonical.sealer;

fn increment(value: i32) -> i32 {
    let result = value + 1;
    return result;
}
)luna";
    auto sealableSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        sealableFunctionSource, "<canonical-sealer>");
    if (!sealableSnapshot.success())
        return fail("frontend rejected the canonical sealer source");
    moon::LunaLowerer sealableLowerer;
    auto sealableModule = sealableLowerer.lower(
        *sealableSnapshot.program(), *sealableSnapshot.symbolTable());
    if (!sealableLowerer.errors().empty() ||
        !verifier.verify(*sealableModule))
        return fail("structured sealer input did not verify");
    moon::FunctionDecl* sealableFunction = nullptr;
    for (auto& declaration : sealableModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "increment") {
            sealableFunction = function;
            break;
        }
    }
    if (!sealableFunction || !sealableFunction->body ||
        sealableFunction->controlFlow)
        return fail("sealer input does not own exactly one construction body");
    moon::Sealer sealer;
    if (!sealer.sealFunctionBodies(*sealableModule)) {
        for (const auto& error : sealer.errors()) std::cerr << error << '\n';
        return fail("atomic function sealing rejected a canonical source body");
    }
    if (sealableFunction->body || !sealableFunction->controlFlow ||
        !verifier.verify(*sealableModule))
        return fail("atomic function sealing did not install one verified CFG body");
    sealableFunction->body = std::make_unique<moon::BlockStmt>();
    if (verifier.verify(*sealableModule))
        return fail("module verifier accepted simultaneous function bodies");
    sealableFunction->body.reset();
    if (!verifier.verify(*sealableModule))
        return fail("restored sealed function module no longer verifies");
    auto sealedParameter = std::find_if(
        sealableFunction->controlFlow->locals.begin(),
        sealableFunction->controlFlow->locals.end(),
        [](const moon::LocalRecord& local) {
            return local.kind == moon::LocalKind::Parameter;
        });
    if (sealedParameter == sealableFunction->controlFlow->locals.end())
        return fail("sealed function lost its canonical parameter");
    sealedParameter->relation = luna::ownership::Relation::SharedBorrow;
    if (verifier.verify(*sealableModule))
        return fail("module verifier accepted a CFG/signature parameter mismatch");
    sealedParameter->relation = luna::ownership::Relation::Owned;
    if (!verifier.verify(*sealableModule))
        return fail("restored canonical function signature no longer verifies");

    const std::string closureSealerSource = R"luna(
fn main() -> i32 {
    let captured = 21;
    let outer = fn() -> i32 {
        let inner = fn() -> i32 {
            return captured * 2;
        };
        return inner();
    };
    return outer();
}
)luna";
    auto closureSealerSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        closureSealerSource, "<closure-sealer>");
    if (!closureSealerSnapshot.success())
        return fail("frontend rejected the closure sealer source");
    moon::LunaLowerer closureSealerLowerer;
    auto closureSealerModule = closureSealerLowerer.lower(
        *closureSealerSnapshot.program(),
        *closureSealerSnapshot.symbolTable());
    moon::Sealer closureSealer;
    if (!closureSealerLowerer.errors().empty() ||
        !closureSealer.sealFunctionBodies(*closureSealerModule) ||
        !verifier.verify(*closureSealerModule)) {
        for (const auto& diagnostic : closureSealerLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : closureSealer.errors())
            std::cerr << error << '\n';
        return fail("closure-bearing function did not survive Sealer cloning");
    }
    const moon::MakeClosureExpr* sealedOuterClosure = nullptr;
    const moon::LambdaExpr* sealedOuterLambda = nullptr;
    const moon::MakeClosureExpr* sealedInnerClosure = nullptr;
    for (const auto& declaration : closureSealerModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || function->name != "main" ||
            !function->controlFlow)
            continue;
        for (const auto& block : function->controlFlow->blocks) {
            for (const auto& operation : block.operations) {
                const auto* let = dynamic_cast<const moon::LetStmt*>(
                    operation.get());
                if (!let || !let->initializer) continue;
                if (const auto* closure = dynamic_cast<
                        const moon::MakeClosureExpr*>(
                        let->initializer.get())) {
                    sealedOuterClosure = closure;
                    sealedOuterLambda = closure->lambda.get();
                }
            }
        }
    }
    if (!sealedOuterClosure || !sealedOuterLambda ||
        !sealedOuterLambda->controlFlow)
        return fail("sealed function dropped its outer closure construction");
    for (const auto& block : sealedOuterLambda->controlFlow->blocks) {
        for (const auto& operation : block.operations) {
            const auto* let = dynamic_cast<const moon::LetStmt*>(
                operation.get());
            if (!let || !let->initializer) continue;
            if (const auto* closure = dynamic_cast<
                    const moon::MakeClosureExpr*>(
                    let->initializer.get()))
                sealedInnerClosure = closure;
        }
    }
    if (!sealedInnerClosure || !sealedInnerClosure->lambda ||
        !sealedInnerClosure->lambda->controlFlow)
        return fail("sealed function dropped its nested closure construction");
    bool sealedEnvLoad = false;
    for (const auto& block : sealedInnerClosure->lambda->controlFlow->blocks) {
        for (const auto& operation : block.operations) {
            if (dynamic_cast<const moon::EnvLoadExpr*>(
                    operation.get()))
                sealedEnvLoad = true;
        }
        if (block.terminator.operand &&
            dynamic_cast<const moon::EnvLoadExpr*>(
                block.terminator.operand.get()))
            sealedEnvLoad = true;
        if (auto* binary = dynamic_cast<const moon::BinaryExpr*>(
                block.terminator.operand.get())) {
            if (dynamic_cast<const moon::EnvLoadExpr*>(
                    binary->lhs.get()) ||
                dynamic_cast<const moon::EnvLoadExpr*>(
                    binary->rhs.get()))
                sealedEnvLoad = true;
        }
    }
    if (!sealedEnvLoad)
        return fail("sealed nested closure lost its environment load");
    if (sealedInnerClosure->capturedValues.size() != 1 ||
        !dynamic_cast<const moon::EnvLoadExpr*>(
            sealedInnerClosure->capturedValues.front().get()))
        return fail("sealed nested closure did not rewrite its transitive capture into an environment load");

    const std::string cfgCodegenSource = R"luna(
package canonical.codegen;

fn main() -> i32 {
    let value = 40;
    if value == 40 {
        let value = 41;
        return value + 1;
    }
    return 0;
}
)luna";
    auto cfgCodegenSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        cfgCodegenSource, "<canonical-codegen>");
    if (!cfgCodegenSnapshot.success())
        return fail("frontend rejected the canonical codegen source");
    moon::LunaLowerer cfgCodegenLowerer;
    auto cfgCodegenModule = cfgCodegenLowerer.lower(
        *cfgCodegenSnapshot.program(), *cfgCodegenSnapshot.symbolTable());
    moon::Sealer cfgCodegenSealer;
    const bool cfgCodegenSealed =
        cfgCodegenSealer.sealFunctionBodies(*cfgCodegenModule);
    const bool cfgCodegenVerified =
        cfgCodegenSealed && verifier.verify(*cfgCodegenModule);
    if (!cfgCodegenLowerer.errors().empty() ||
        !cfgCodegenSealed || !cfgCodegenVerified) {
        for (const auto& diagnostic : cfgCodegenLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : cfgCodegenSealer.errors())
            std::cerr << error << '\n';
        if (cfgCodegenSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic << '\n';
        return fail("canonical codegen fixture did not seal and verify");
    }
    CodeGenerator cfgCodegen("canonical.codegen");
    if (!cfgCodegen.generate(cfgCodegenModule.get())) {
        for (const auto& diagnostic : cfgCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected the initial canonical CFG slice");
    }
    if (cfgCodegen.jitRun() != 42)
        return fail("canonical CFG JIT did not preserve branch/local semantics");

    const std::string cfgSwitchCodegenSource = R"luna(
package canonical.switch_codegen;

enum Choice {
    None;
    Some(i32);
}

fn inspect_choice(value: Choice) -> i32 {
    match value {
        Choice::None => {
            return 0;
        },
        Choice::Some(number) => {
            return number;
        }
    }
}

fn inspect_result(value: Result<i32, i32>) -> i32 {
    match value {
        Ok(number) => {
            return number;
        },
        Err(error) => {
            return error;
        }
    }
}

fn main() -> i32 {
    return inspect_choice(Choice::Some(19))
        + inspect_result(Ok::<i32, i32>(23));
}
)luna";
    auto cfgSwitchSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            cfgSwitchCodegenSource, "<canonical-switch-codegen>");
    if (!cfgSwitchSnapshot.success())
        return fail("frontend rejected the canonical switch codegen source");
    moon::LunaLowerer cfgSwitchLowerer;
    auto cfgSwitchModule = cfgSwitchLowerer.lower(
        *cfgSwitchSnapshot.program(), *cfgSwitchSnapshot.symbolTable());
    moon::Sealer cfgSwitchSealer;
    const bool cfgSwitchSealed =
        cfgSwitchSealer.sealFunctionBodies(*cfgSwitchModule);
    const bool cfgSwitchVerified =
        cfgSwitchSealed && verifier.verify(*cfgSwitchModule);
    if (!cfgSwitchLowerer.errors().empty() ||
        !cfgSwitchSealed || !cfgSwitchVerified) {
        for (const auto& diagnostic : cfgSwitchLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : cfgSwitchSealer.errors())
            std::cerr << error << '\n';
        if (cfgSwitchSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic << '\n';
        return fail("canonical switch codegen fixture did not seal and verify");
    }
    size_t switchCount = 0;
    size_t bindingCount = 0;
    for (const auto& declaration : cfgSwitchModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || !function->controlFlow) continue;
        for (const auto& block : function->controlFlow->blocks) {
            if (block.terminator.kind != moon::TerminatorKind::Switch)
                continue;
            ++switchCount;
            for (const auto& item : block.terminator.cases)
                bindingCount += item.bindings.size();
        }
    }
    if (switchCount != 2 || bindingCount != 3)
        return fail("canonical switch fixture lost enum/Result pattern bindings");
    CodeGenerator cfgSwitchCodegen("canonical.switch_codegen");
    if (!cfgSwitchCodegen.generate(cfgSwitchModule.get())) {
        for (const auto& diagnostic : cfgSwitchCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected canonical enum/Result switches");
    }
    if (cfgSwitchCodegen.jitRun() != 42)
        return fail("canonical switch JIT did not preserve payload bindings");

    const std::string cfgCleanupCodegenSource = R"luna(
package canonical.cleanup_codegen;

fn release(affine value: string) -> i32 {
    return 7;
}

fn branch_release(flag: bool) -> i32 {
    if flag {
        let value = "temporary";
        value;
    }
    return 3;
}
)luna";
    auto cfgCleanupSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            cfgCleanupCodegenSource, "<canonical-cleanup-codegen>");
    if (!cfgCleanupSnapshot.success())
        return fail("frontend rejected the canonical cleanup codegen source");
    moon::LunaLowerer cfgCleanupLowerer;
    auto cfgCleanupModule = cfgCleanupLowerer.lower(
        *cfgCleanupSnapshot.program(), *cfgCleanupSnapshot.symbolTable());
    moon::Sealer cfgCleanupSealer;
    if (!cfgCleanupLowerer.errors().empty() ||
        !cfgCleanupSealer.sealFunctionBodies(*cfgCleanupModule) ||
        !verifier.verify(*cfgCleanupModule))
        return fail("canonical cleanup codegen fixture did not seal and verify");
    const moon::FunctionDecl* cleanupFunction = nullptr;
    const moon::FunctionDecl* branchCleanupFunction = nullptr;
    for (const auto& declaration : cfgCleanupModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "release") {
            cleanupFunction = function;
        } else if (function && function->name == "branch_release") {
            branchCleanupFunction = function;
        }
    }
    bool hasReturnCleanup = false;
    bool hasBranchCleanup = false;
    if (cleanupFunction && cleanupFunction->controlFlow)
        for (const auto& block : cleanupFunction->controlFlow->blocks)
            hasReturnCleanup = hasReturnCleanup ||
                (block.terminator.kind == moon::TerminatorKind::Return &&
                 !block.terminator.exitCleanups.empty());
    if (branchCleanupFunction && branchCleanupFunction->controlFlow)
        for (const auto& block : branchCleanupFunction->controlFlow->blocks)
            hasBranchCleanup = hasBranchCleanup ||
                !block.terminator.primary.cleanups.empty() ||
                !block.terminator.secondary.cleanups.empty();
    if (!hasReturnCleanup || !hasBranchCleanup)
        return fail("cleanup codegen fixture has no canonical cleanup edge");
    CodeGenerator cfgCleanupCodegen("canonical.cleanup_codegen");
    if (!cfgCleanupCodegen.generate(cfgCleanupModule.get())) {
        for (const auto& diagnostic : cfgCleanupCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected a canonical root cleanup edge");
    }

    moon::Module compositionModule;
    compositionModule.name = "canonical.composition";
    const auto compositionUnit = compositionModule.registerType(TyUnit);
    const auto compositionI32 = compositionModule.registerType(TyI32);
    const auto sharedI32Type = Type::makeReference(TyI32);
    const auto sharedI32TypeId =
        compositionModule.registerType(sharedI32Type);
    const auto interceptorType = Type::makeFragment(
        {sharedI32Type}, TyUnit, false, ContinuationKind::Interceptor,
        {{luna::ownership::Relation::SharedBorrow,
          luna::ownership::Usage::Copy}});
    const auto interceptorTypeId =
        compositionModule.registerType(interceptorType);
    const std::string interceptorId =
        "canonical.composition::fragment::guard";
    moon::DeclarationRecord interceptorRecord;
    interceptorRecord.id = interceptorId;
    interceptorRecord.familyId = interceptorId;
    interceptorRecord.symbolId =
        luna::identity::symbolIdFromCanonical(interceptorId);
    interceptorRecord.sourceName = "guard";
    interceptorRecord.linkageName = "guard";
    interceptorRecord.kind = moon::DeclarationKind::Fragment;
    interceptorRecord.type = interceptorTypeId;
    interceptorRecord.sysmeta = interceptorType->sysmeta;
    compositionModule.declarationTable.push_back(interceptorRecord);

    auto interceptor = std::make_unique<moon::FragmentDecl>();
    interceptor->packageId = compositionModule.name;
    interceptor->declarationId = interceptorId;
    interceptor->familyId = interceptorId;
    interceptor->symbolId = interceptorRecord.symbolId;
    interceptor->name = "guard";
    interceptor->generatedSymbolName = "guard";
    interceptor->kind = moon::FragmentKind::Interceptor;
    interceptor->cardinality = moon::FragmentCardinality::Once;
    interceptor->structuralType = interceptorTypeId;
    interceptor->params.push_back({
        "view", false, luna::ownership::Usage::Copy,
        luna::ownership::Relation::SharedBorrow, sharedI32TypeId});
    interceptor->body = std::make_unique<moon::BlockStmt>();
    auto guardedReturn = std::make_unique<moon::IfStmt>();
    auto returnCondition = std::make_unique<moon::BoolLiteralExpr>();
    returnCondition->value = false;
    returnCondition->type = compositionModule.registerType(TyBool);
    guardedReturn->cond = std::move(returnCondition);
    guardedReturn->thenBlock = std::make_unique<moon::BlockStmt>();
    guardedReturn->thenBlock->stmts.push_back(
        std::make_unique<moon::ReturnStmt>());
    interceptor->body->stmts.push_back(std::move(guardedReturn));
    auto guardedAbort = std::make_unique<moon::IfStmt>();
    auto abortCondition = std::make_unique<moon::BoolLiteralExpr>();
    abortCondition->value = true;
    abortCondition->type = compositionModule.registerType(TyBool);
    guardedAbort->cond = std::move(abortCondition);
    guardedAbort->thenBlock = std::make_unique<moon::BlockStmt>();
    guardedAbort->thenBlock->stmts.push_back(
        std::make_unique<moon::AbortStmt>());
    interceptor->body->stmts.push_back(std::move(guardedAbort));
    auto* interceptorBody = interceptor->body.get();
    compositionModule.declarations.push_back(std::move(interceptor));

    const auto contextType = Type::makeFragment(
        {}, TyUnit, false, ContinuationKind::Context);
    const auto contextTypeId = compositionModule.registerType(contextType);
    const std::string contextId =
        "canonical.composition::fragment::around";
    moon::DeclarationRecord contextRecord;
    contextRecord.id = contextId;
    contextRecord.familyId = contextId;
    contextRecord.symbolId =
        luna::identity::symbolIdFromCanonical(contextId);
    contextRecord.sourceName = "around";
    contextRecord.linkageName = "around";
    contextRecord.kind = moon::DeclarationKind::Fragment;
    contextRecord.type = contextTypeId;
    contextRecord.sysmeta = contextType->sysmeta;
    compositionModule.declarationTable.push_back(contextRecord);
    auto context = std::make_unique<moon::FragmentDecl>();
    context->packageId = compositionModule.name;
    context->declarationId = contextId;
    context->familyId = contextId;
    context->symbolId = contextRecord.symbolId;
    context->name = "around";
    context->generatedSymbolName = "around";
    context->kind = moon::FragmentKind::Context;
    context->cardinality = moon::FragmentCardinality::Once;
    context->structuralType = contextTypeId;
    context->body = std::make_unique<moon::BlockStmt>();
    auto shadow = std::make_unique<moon::LetStmt>();
    shadow->name = "value";
    shadow->type = compositionI32;
    auto shadowValue = std::make_unique<moon::IntLiteralExpr>();
    shadowValue->value = 2;
    shadowValue->type = compositionI32;
    shadow->initializer = std::move(shadowValue);
    context->body->stmts.push_back(std::move(shadow));
    context->body->stmts.push_back(std::make_unique<moon::ResumeStmt>());
    auto postResumeEffect = std::make_unique<moon::ExprStmt>();
    auto postResumeUnit = std::make_unique<moon::UnitExpr>();
    postResumeUnit->type = compositionUnit;
    postResumeEffect->expr = std::move(postResumeUnit);
    context->body->stmts.push_back(std::move(postResumeEffect));
    auto* contextBody = context->body.get();
    compositionModule.declarations.push_back(std::move(context));
    compositionModule.sealTypeTable();
    const auto* sealedInterceptor =
        compositionModule.findDeclarationById(interceptorId);
    if (!sealedInterceptor)
        return fail("static composition lost its fragment declaration row");
    auto* executableInterceptor = static_cast<moon::FragmentDecl*>(
        compositionModule.declarations.front().get());
    executableInterceptor->contractId = sealedInterceptor->contractId;
    executableInterceptor->sysmeta = sealedInterceptor->sysmeta;
    const auto* sealedContext =
        compositionModule.findDeclarationById(contextId);
    if (!sealedContext)
        return fail("static composition lost its context declaration row");
    auto* executableContext = static_cast<moon::FragmentDecl*>(
        compositionModule.declarations[1].get());
    executableContext->contractId = sealedContext->contractId;
    executableContext->sysmeta = sealedContext->sysmeta;
    compositionModule.rebuildIndexes();
    const moon::DeclarationRef interceptorRef{
        sealedInterceptor->symbolId, sealedInterceptor->contractId};
    const moon::DeclarationRef contextRef{
        sealedContext->symbolId, sealedContext->contractId};

    auto compositionBody = std::make_unique<moon::BlockStmt>();
    auto borrowedSource = std::make_unique<moon::LetStmt>();
    borrowedSource->name = "source";
    borrowedSource->type = compositionI32;
    auto borrowedValue = std::make_unique<moon::IntLiteralExpr>();
    borrowedValue->value = 7;
    borrowedValue->type = compositionI32;
    borrowedSource->initializer = std::move(borrowedValue);
    compositionBody->stmts.push_back(std::move(borrowedSource));
    auto staticApply = std::make_unique<moon::ApplyStmt>();
    staticApply->slotName = "hook";
    staticApply->fragmentName = "guard";
    staticApply->fragmentRef = interceptorRef;
    staticApply->body = std::make_unique<moon::BlockStmt>();
    auto slotInvocation = std::make_unique<moon::SlotInvokeStmt>();
    slotInvocation->name = "hook";
    slotInvocation->acceptedKind = moon::FragmentKind::Interceptor;
    slotInvocation->acceptedCardinality = moon::FragmentCardinality::Once;
    auto sharedArgument = std::make_unique<moon::BorrowExpr>();
    sharedArgument->type = sharedI32TypeId;
    auto sharedSource = std::make_unique<moon::IdentifierExpr>();
    sharedSource->name = "source";
    sharedSource->type = compositionI32;
    sharedArgument->operand = std::move(sharedSource);
    slotInvocation->args.push_back(std::move(sharedArgument));
    slotInvocation->continuation = std::make_unique<moon::BlockStmt>();
    auto continuationEffect = std::make_unique<moon::ExprStmt>();
    auto continuationUnit = std::make_unique<moon::UnitExpr>();
    continuationUnit->type = compositionUnit;
    continuationEffect->expr = std::move(continuationUnit);
    slotInvocation->continuation->stmts.push_back(
        std::move(continuationEffect));
    staticApply->body->stmts.push_back(std::move(slotInvocation));
    compositionBody->stmts.push_back(std::move(staticApply));

    auto compositionCfg = cfgBuilder.build(
        std::move(compositionBody), {}, moon::RegionKind::Function,
        compositionModule);
    size_t applyRegions = 0;
    size_t fragmentRegions = 0;
    size_t continuationRegions = 0;
    size_t resumeEdges = 0;
    size_t abortEdges = 0;
    const moon::LocalRecord* borrowedFragmentLocal = nullptr;
    bool borrowedFragmentCleanup = false;
    for (const auto& region : compositionCfg
             ? compositionCfg->regions
             : std::vector<moon::RegionRecord>{}) {
        applyRegions += region.kind == moon::RegionKind::Apply;
        fragmentRegions += region.kind == moon::RegionKind::Fragment;
        continuationRegions += region.kind == moon::RegionKind::Continuation;
    }
    if (compositionCfg) {
        for (const auto& local : compositionCfg->locals)
            if (local.name == "view") borrowedFragmentLocal = &local;
        if (borrowedFragmentLocal)
            for (const auto& cleanup : compositionCfg->cleanups)
                borrowedFragmentCleanup = borrowedFragmentCleanup ||
                    cleanup.place.root == borrowedFragmentLocal->id;
        for (const auto& block : compositionCfg->blocks) {
            resumeEdges +=
                block.terminator.kind == moon::TerminatorKind::Resume;
            abortEdges +=
                block.terminator.kind == moon::TerminatorKind::Abort;
        }
    }
    if (!compositionCfg ||
        !cfgVerifier.verify(*compositionCfg, compositionModule) ||
        applyRegions != 1 || fragmentRegions != 1 ||
        continuationRegions != 1 || resumeEdges != 0 || abortEdges != 1 ||
        !borrowedFragmentLocal ||
        borrowedFragmentLocal->relation !=
            luna::ownership::Relation::SharedBorrow ||
        borrowedFragmentCleanup ||
        executableInterceptor->body.get() != interceptorBody)
        return fail("static interceptor did not compose into canonical CFG regions and edges");
    moon::LetStmt* borrowedFragmentDefinition = nullptr;
    for (auto& block : compositionCfg->blocks)
        for (auto& operation : block.operations)
            if (auto* declaration = dynamic_cast<moon::LetStmt*>(
                    operation.get());
                declaration && borrowedFragmentLocal &&
                declaration->local == borrowedFragmentLocal->id)
                borrowedFragmentDefinition = declaration;
    if (!borrowedFragmentDefinition)
        return fail("static composition lost its fragment parameter definition");
    const auto borrowedFragmentId = borrowedFragmentLocal->id;
    compositionCfg->locals[borrowedFragmentId.value].relation =
        luna::ownership::Relation::Owned;
    borrowedFragmentDefinition->relation =
        luna::ownership::Relation::Owned;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted a forged fragment parameter relation");
    compositionCfg->locals[borrowedFragmentId.value].relation =
        luna::ownership::Relation::SharedBorrow;
    borrowedFragmentDefinition->relation =
        luna::ownership::Relation::SharedBorrow;
    if (!cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("restored fragment parameter relation no longer verifies");
    moon::BlockId interceptorContinuationEntry;
    for (const auto& composedRegion : compositionCfg->regions)
        if (composedRegion.kind == moon::RegionKind::Continuation)
            interceptorContinuationEntry = composedRegion.entry;
    moon::Terminator* automaticForward = nullptr;
    moon::Terminator* composedAbort = nullptr;
    for (auto& block : compositionCfg->blocks) {
        if (block.terminator.kind == moon::TerminatorKind::Jump &&
            block.terminator.primary.target == interceptorContinuationEntry)
            automaticForward = &block.terminator;
        if (block.terminator.kind == moon::TerminatorKind::Abort)
            composedAbort = &block.terminator;
    }
    if (!automaticForward || !composedAbort)
        return fail("static composition lost a control terminator");
    const auto fragmentExit = composedAbort->primary.target;
    automaticForward->kind = moon::TerminatorKind::Resume;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted explicit resume in an interceptor");
    automaticForward->kind = moon::TerminatorKind::Jump;
    composedAbort->primary.target = interceptorContinuationEntry;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted abort into the continuation");
    composedAbort->primary.target = fragmentExit;
    if (!cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("restored static composition no longer verifies");

    auto contextCompositionBody = std::make_unique<moon::BlockStmt>();
    auto outerValue = std::make_unique<moon::LetStmt>();
    outerValue->name = "value";
    outerValue->type = compositionI32;
    auto outerInitializer = std::make_unique<moon::IntLiteralExpr>();
    outerInitializer->value = 1;
    outerInitializer->type = compositionI32;
    outerValue->initializer = std::move(outerInitializer);
    contextCompositionBody->stmts.push_back(std::move(outerValue));
    auto contextApply = std::make_unique<moon::ApplyStmt>();
    contextApply->slotName = "hook";
    contextApply->fragmentName = "around";
    contextApply->fragmentRef = contextRef;
    contextApply->body = std::make_unique<moon::BlockStmt>();
    auto contextInvocation = std::make_unique<moon::SlotInvokeStmt>();
    contextInvocation->name = "hook";
    contextInvocation->acceptedKind = moon::FragmentKind::Context;
    contextInvocation->acceptedCardinality = moon::FragmentCardinality::Once;
    contextInvocation->continuation = std::make_unique<moon::BlockStmt>();
    auto outerUse = std::make_unique<moon::ExprStmt>();
    auto outerIdentifier = std::make_unique<moon::IdentifierExpr>();
    outerIdentifier->name = "value";
    outerIdentifier->type = compositionI32;
    outerUse->expr = std::move(outerIdentifier);
    contextInvocation->continuation->stmts.push_back(
        std::move(outerUse));
    contextApply->body->stmts.push_back(std::move(contextInvocation));
    contextCompositionBody->stmts.push_back(std::move(contextApply));

    auto contextCfg = cfgBuilder.build(
        std::move(contextCompositionBody), {},
        moon::RegionKind::Function, compositionModule);
    moon::RegionRecord* contextRegion = nullptr;
    moon::Terminator* contextResume = nullptr;
    moon::Terminator* contextAbort = nullptr;
    moon::IdentifierExpr* continuationIdentifier = nullptr;
    if (contextCfg) {
        for (auto& composedRegion : contextCfg->regions)
            if (composedRegion.kind == moon::RegionKind::Fragment)
                contextRegion = &composedRegion;
        for (auto& block : contextCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Resume)
                contextResume = &block.terminator;
            if (block.terminator.kind == moon::TerminatorKind::Abort)
                contextAbort = &block.terminator;
            if (contextCfg->regions[block.region.value].kind ==
                    moon::RegionKind::Continuation) {
                for (auto& operation : block.operations) {
                    auto* effect = dynamic_cast<moon::ExprStmt*>(
                        operation.get());
                    if (effect)
                        continuationIdentifier =
                            dynamic_cast<moon::IdentifierExpr*>(
                                effect->expr.get());
                }
            }
        }
    }
    if (!contextCfg ||
        !cfgVerifier.verify(*contextCfg, compositionModule) ||
        !contextRegion || !contextResume || !contextAbort ||
        !continuationIdentifier || continuationIdentifier->local.empty() ||
        contextCfg->locals[continuationIdentifier->local.value].scope !=
            contextCfg->rootScope ||
        executableContext->body.get() != contextBody)
        return fail("static context did not preserve its stack continuation boundary");

    moon::LocalId fragmentShadow;
    for (const auto& local : contextCfg->locals)
        if (local.name == "value" && local.scope != contextCfg->rootScope)
            fragmentShadow = local.id;
    if (fragmentShadow.empty())
        return fail("context composition lost its fragment-local shadow");
    const auto outerLocal = continuationIdentifier->local;
    continuationIdentifier->local = fragmentShadow;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier exposed fragment-local state to its continuation");
    continuationIdentifier->local = outerLocal;
    const auto contextContinuationEntry = contextResume->primary.target;
    contextResume->primary.target = contextAbort->primary.target;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted context resume into the fragment exit");
    contextResume->primary.target = contextContinuationEntry;
    contextResume->kind = moon::TerminatorKind::Jump;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted ordinary context jump into a continuation");
    contextResume->kind = moon::TerminatorKind::Resume;
    contextRegion->fragment = interceptorRef;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted context control under an interceptor contract");
    contextRegion->fragment = contextRef;
    if (!cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("restored static context no longer verifies");

    auto blocklessBody = std::make_unique<moon::BlockStmt>();
    auto blocklessApply = std::make_unique<moon::ApplyStmt>();
    blocklessApply->slotName = "hook";
    blocklessApply->fragmentName = "guard";
    blocklessApply->fragmentRef = interceptorRef;
    blocklessBody->stmts.push_back(std::move(blocklessApply));
    if (cfgBuilder.build(
            std::move(blocklessBody), {}, moon::RegionKind::Function,
            compositionModule))
        return fail("canonical builder accepted legacy blockless apply");

    const std::string loweredCompositionSource = R"luna(
package canonical.integration;

context passthrough(value: i32) {
    resume();
}

context lexical_capture {
    outer;
    resume();
}

fn main() -> i32 {
    let outer = 7;
    slot context hook(value: i32) default passthrough;
    hook(outer) {
        outer;
    }
    apply captured(lexical_capture) {
        slot context captured {
            outer;
        }
    }
    return 0;
}
)luna";
    auto compositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredCompositionSource, "<canonical-composition>");
    if (!compositionSnapshot.success()) {
        for (const auto& diagnostic : compositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected canonical default-fragment integration source");
    }
    moon::LunaLowerer integrationLowerer;
    auto integrationModule = integrationLowerer.lower(
        *compositionSnapshot.program(), *compositionSnapshot.symbolTable());
    if (!integrationLowerer.errors().empty()) {
        for (const auto& diagnostic : integrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected canonical default-fragment source");
    }
    if (!verifier.verify(*integrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered default-fragment module failed structured verification");
    }
    moon::FunctionDecl* integrationMain = nullptr;
    const moon::FragmentDecl* integrationFragment = nullptr;
    const moon::FragmentDecl* integrationCaptureFragment = nullptr;
    for (auto& declaration : integrationModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get());
            function && function->name == "main")
            integrationMain = function;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "passthrough")
            integrationFragment = fragment;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "lexical_capture")
            integrationCaptureFragment = fragment;
    }
    if (!integrationMain || !integrationMain->body ||
        !integrationFragment || !integrationFragment->body ||
        !integrationCaptureFragment || !integrationCaptureFragment->body)
        return fail("lowered integration module lost main or its default fragment");
    const auto* integrationFragmentBody = integrationFragment->body.get();
    const auto* integrationCaptureBody =
        integrationCaptureFragment->body.get();
    moon::ControlFlowBuilder integrationBuilder;
    auto integrationCfg = integrationBuilder.build(
        std::move(integrationMain->body), integrationMain->params,
        moon::RegionKind::Function, *integrationModule);
    if (!integrationCfg) {
        for (const auto& error : integrationBuilder.errors())
            std::cerr << error << '\n';
        return fail("lowered default fragment did not enter canonical CFG construction");
    }
    if (!cfgVerifier.verify(*integrationCfg, *integrationModule)) {
        for (const auto& diagnostic : cfgVerifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered default fragment failed canonical CFG verification");
    }
    size_t integrationApplyRegions = 0;
    size_t integrationFragmentRegions = 0;
    size_t integrationContinuationRegions = 0;
    size_t integrationResumeEdges = 0;
    size_t fragmentOuterCaptures = 0;
    size_t continuationOuterCaptures = 0;
    bool captureEscapedRoot = false;
    for (const auto& composedRegion : integrationCfg->regions) {
        integrationApplyRegions +=
            composedRegion.kind == moon::RegionKind::Apply;
        integrationFragmentRegions +=
            composedRegion.kind == moon::RegionKind::Fragment;
        integrationContinuationRegions +=
            composedRegion.kind == moon::RegionKind::Continuation;
    }
    for (auto& block : integrationCfg->blocks) {
        integrationResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
        for (auto& operation : block.operations) {
            auto* effect = dynamic_cast<moon::ExprStmt*>(operation.get());
            auto* identifier = effect
                ? dynamic_cast<moon::IdentifierExpr*>(effect->expr.get())
                : nullptr;
            if (!identifier || identifier->name != "outer") continue;
            if (identifier->local.empty() ||
                integrationCfg->locals[identifier->local.value].name !=
                    "outer" ||
                integrationCfg->locals[identifier->local.value].scope !=
                    integrationCfg->rootScope)
                captureEscapedRoot = true;
            const auto kind =
                integrationCfg->regions[block.region.value].kind;
            fragmentOuterCaptures += kind == moon::RegionKind::Fragment;
            continuationOuterCaptures +=
                kind == moon::RegionKind::Continuation;
        }
    }
    if (integrationApplyRegions != 2 ||
        integrationFragmentRegions != 2 ||
        integrationContinuationRegions != 2 ||
        integrationResumeEdges != 2 ||
        fragmentOuterCaptures != 1 ||
        continuationOuterCaptures != 2 || captureEscapedRoot ||
        integrationFragment->body.get() != integrationFragmentBody ||
        integrationCaptureFragment->body.get() != integrationCaptureBody)
        return fail("frontend-to-CFG composition lost its default, capture, or construction body");

    const std::string loweredRuntimeCompositionSource = R"luna(
package canonical.runtime_boundary;

runtime context trace(value: i32) {
    value;
    resume();
}

fn stable_entry() -> i32 {
    return 1;
}

fn dynamic_entry() -> i32 {
    dynamic slot context pipeline(value: i32);
    dynamic apply pipeline(trace) {
        pipeline(1) {
            2;
        }
    }
    return 0;
}
)luna";
    auto runtimeCompositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredRuntimeCompositionSource,
            "<canonical-runtime-composition>");
    if (!runtimeCompositionSnapshot.success()) {
        for (const auto& diagnostic : runtimeCompositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the canonical runtime-boundary source");
    }
    moon::LunaLowerer runtimeIntegrationLowerer;
    auto runtimeIntegrationModule = runtimeIntegrationLowerer.lower(
        *runtimeCompositionSnapshot.program(),
        *runtimeCompositionSnapshot.symbolTable());
    if (!runtimeIntegrationLowerer.errors().empty()) {
        for (const auto& diagnostic : runtimeIntegrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the runtime-boundary source");
    }
    if (!verifier.verify(*runtimeIntegrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("runtime-boundary module failed structured verification");
    }
    moon::FunctionDecl* dynamicEntry = nullptr;
    moon::FunctionDecl* stableEntry = nullptr;
    for (auto& declaration : runtimeIntegrationModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "dynamic_entry") {
            dynamicEntry = function;
        } else if (function && function->name == "stable_entry") {
            stableEntry = function;
        }
    }
    if (!dynamicEntry || !dynamicEntry->body ||
        !stableEntry || !stableEntry->body)
        return fail("runtime-boundary module lost its entry body");
    const auto* dynamicConstructionBody = dynamicEntry->body.get();
    const auto* stableConstructionBody = stableEntry->body.get();
    moon::Sealer runtimeBoundarySealer;
    if (runtimeBoundarySealer.sealFunctionBodies(
            *runtimeIntegrationModule))
        return fail("canonical function sealing accepted a lowered runtime apply");
    const bool diagnosedRuntimeBoundary = std::any_of(
        runtimeBoundarySealer.errors().begin(),
        runtimeBoundarySealer.errors().end(),
        [](const std::string& error) {
            return error.find(
                       "runtime slot composition is outside the static canonical CFG slice") !=
                       std::string::npos ||
                error.find(
                    "runtime apply is outside the static canonical CFG slice") !=
                    std::string::npos;
        });
    if (!diagnosedRuntimeBoundary)
        return fail("canonical CFG did not diagnose its runtime-apply boundary");
    if (dynamicEntry->body.get() != dynamicConstructionBody ||
        dynamicEntry->controlFlow ||
        stableEntry->body.get() != stableConstructionBody ||
        stableEntry->controlFlow)
        return fail("failed module sealing partially consumed its function set");
    if (!verifier.verify(*runtimeIntegrationModule))
        return fail("failed atomic sealing changed the structured module");

    const auto reverseIterator = reverse.typesById.find(shortId.value);
    if (reverseIterator == reverse.typesById.end())
        return fail("sealed type index lost the iterator type");
    reverse.typeTable[reverseIterator->second].sysmeta.resource.usage =
        luna::ownership::Usage::Copy;
    if (verifier.verify(reverse))
        return fail("verifier accepted a forged derived Resource contract");

    moon::Module symbols;
    symbols.name = "canonical.symbols";
    const auto calleeType = Type::makeFunction({TyI32}, TyI32);
    const auto callerType = Type::makeFunction({}, TyUnit);
    const auto calleeTypeRef = symbols.registerType(calleeType);
    const auto callerTypeRef = symbols.registerType(callerType);
    auto addFunctionRecord = [&](const std::string& id,
                                 const std::string& linkage,
                                 const TypePtr& type,
                                 const moon::TypeRef& typeReference) {
        moon::DeclarationRecord record;
        record.id = id;
        record.familyId = id;
        record.symbolId = luna::identity::symbolIdFromCanonical(id);
        record.sourceName = linkage;
        record.linkageName = linkage;
        record.kind = moon::DeclarationKind::Function;
        record.type = typeReference;
        record.sysmeta = type->sysmeta;
        record.canonicalContract = moon::canonicalContract(record);
        record.contractId = luna::identity::contractIdFromCanonical(
            record.canonicalContract);
        record.sysmeta.identity.symbol = record.symbolId;
        record.sysmeta.identity.contract = record.contractId;
        symbols.declarationTable.push_back(std::move(record));
    };
    const std::string calleeId =
        "canonical.symbols::fn::callee";
    const std::string callerId =
        "canonical.symbols::fn::caller";
    addFunctionRecord(calleeId, "callee", calleeType, calleeTypeRef);
    addFunctionRecord(callerId, "caller", callerType, callerTypeRef);

    auto caller = std::make_unique<moon::FunctionDecl>();
    caller->packageId = symbols.name;
    caller->declarationId = callerId;
    caller->familyId = callerId;
    caller->symbolId = luna::identity::symbolIdFromCanonical(callerId);
    caller->name = "caller";
    caller->generatedSymbolName = "caller";
    caller->returnType = symbols.registerType(TyUnit);
    caller->body = std::make_unique<moon::BlockStmt>();
    auto statement = std::make_unique<moon::ExprStmt>();
    auto call = std::make_unique<moon::CallExpr>();
    auto callee = std::make_unique<moon::IdentifierExpr>();
    callee->name = "callee";
    callee->type = calleeTypeRef;
    call->callee = std::move(callee);
    auto argument = std::make_unique<moon::IntLiteralExpr>();
    argument->value = 1;
    argument->type = symbols.registerType(TyI32);
    call->args.push_back(std::move(argument));
    call->type = symbols.registerType(TyI32);
    auto* loweredCall = call.get();
    statement->expr = std::move(call);
    caller->body->stmts.push_back(std::move(statement));
    symbols.declarations.push_back(std::move(caller));
    symbols.sealTypeTable();

    const auto* calleeRecord = symbols.findDeclarationById(calleeId);
    const auto* callerRecord = symbols.findDeclarationById(callerId);
    if (!calleeRecord || !callerRecord)
        return fail("sealed declaration table lost stable symbol rows");
    loweredCall->calleeRef = {
        calleeRecord->symbolId, calleeRecord->contractId};
    auto* loweredIdentifier = static_cast<moon::IdentifierExpr*>(
        loweredCall->callee.get());
    loweredIdentifier->declaration = loweredCall->calleeRef;
    symbols.declarations.front()->contractId = callerRecord->contractId;
    if (!verifier.verify(symbols))
        return fail("stable declaration references failed independent verification");
    loweredCall->calleeRef.contract = {"contract_forged"};
    if (verifier.verify(symbols))
        return fail("verifier accepted a forged call-site ContractId");

    auto productRecord = module.typesById.find(productId.value);
    if (productRecord == module.typesById.end())
        return fail("sealed type index lost the product type");
    module.typeTable[productRecord->second].fields.clear();
    if (verifier.verify(module))
        return fail("verifier accepted a frozen payload inconsistent with its TypeId");

    // Non-Copy capture sealer: a lambda capturing an Affine (string) binding
    // must survive Sealer cloning and canonical CFG construction. The
    // OwnershipChecker attaches an implicit FreeStmt for the captured local at
    // lambda-body exit; canonical construction must suppress that cleanup
    // because the environment is owned by the closure value, not by the
    // lambda function (C016 CL010).
    const std::string affineCaptureSealerSource = R"luna(
fn use_string(s: string) -> i32 {
    return 0;
}
fn main() -> i32 {
    let captured = "luna";
    let read = fn() -> i32 {
        return use_string(captured);
    };
    return read();
}
)luna";
    auto affineCaptureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        affineCaptureSealerSource, "<affine-capture-sealer>");
    if (!affineCaptureSnapshot.success())
        return fail("frontend rejected the affine capture sealer source");
    moon::LunaLowerer affineCaptureLowerer;
    auto affineCaptureModule = affineCaptureLowerer.lower(
        *affineCaptureSnapshot.program(),
        *affineCaptureSnapshot.symbolTable());
    if (!affineCaptureLowerer.errors().empty()) {
        for (const auto& diagnostic : affineCaptureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the affine capture source");
    }
    moon::Sealer affineCaptureSealer;
    if (!affineCaptureSealer.sealFunctionBodies(*affineCaptureModule) ||
        !verifier.verify(*affineCaptureModule)) {
        for (const auto& error : affineCaptureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("affine capture function did not survive Sealer canonicalization");
    }
    // Verify the sealed lambda CFG does not retain an implicit FreeStmt for
    // the captured local as an operation block (it must be suppressed).
    bool sealedLambdaHasFreeStmt = false;
    for (const auto& declaration : affineCaptureModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || !function->controlFlow) continue;
        for (const auto& block : function->controlFlow->blocks) {
            for (const auto& operation : block.operations) {
                if (dynamic_cast<const moon::FreeStmt*>(operation.get()))
                    sealedLambdaHasFreeStmt = true;
            }
        }
    }
    if (sealedLambdaHasFreeStmt)
        return fail("sealed canonical CFG retained an implicit FreeStmt operation");

    // Materialized iterator recipe: a simple for-each over a Copy array must
    // survive Sealer canonicalization. The for-loop's into_iter() creates a
    // materialized recipe that the canonical CFG must expand into ordinary
    // locals, indexed access, and a backedge graph.
    const std::string iteratorSealerSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    for v in values {
        total = total + v;
    }
    return total;
}
)luna";
    auto iteratorSealerSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        iteratorSealerSource, "<iterator-sealer>");
    if (!iteratorSealerSnapshot.success())
        return fail("frontend rejected the iterator sealer source");
    moon::LunaLowerer iteratorSealerLowerer;
    auto iteratorSealerModule = iteratorSealerLowerer.lower(
        *iteratorSealerSnapshot.program(),
        *iteratorSealerSnapshot.symbolTable());
    if (!iteratorSealerLowerer.errors().empty()) {
        for (const auto& diagnostic : iteratorSealerLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the iterator sealer source");
    }
    moon::Sealer iteratorSealer;
    if (!iteratorSealer.sealFunctionBodies(*iteratorSealerModule) ||
        !verifier.verify(*iteratorSealerModule)) {
        for (const auto& error : iteratorSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Copy array for-each did not survive Sealer canonicalization");
    }

    // Capturing-closure iterator adapter: a map adapter that captures a Copy
    // local must survive Sealer canonicalization. The canonical CFG must
    // expand the map as an ordinary closure call with an environment pointer
    // rather than rejecting it as "capture-free callable only".
    const std::string capturingIterSource = R"luna(
fn main() -> i32 {
    let offset = 1;
    let values = [1, 2, 3, 4];
    let total = 0;
    for v in values.iter().map(fn(x: &i32) -> i32 { return *x + offset; }) {
        total = total + v;
    }
    return total;
}
)luna";
    auto capturingIterSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        capturingIterSource, "<capturing-iter>");
    if (!capturingIterSnapshot.success())
        return fail("frontend rejected the capturing iterator source");
    moon::LunaLowerer capturingIterLowerer;
    auto capturingIterModule = capturingIterLowerer.lower(
        *capturingIterSnapshot.program(),
        *capturingIterSnapshot.symbolTable());
    if (!capturingIterLowerer.errors().empty()) {
        for (const auto& diagnostic : capturingIterLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the capturing iterator source");
    }
    moon::Sealer capturingIterSealer;
    if (!capturingIterSealer.sealFunctionBodies(*capturingIterModule) ||
        !verifier.verify(*capturingIterModule)) {
        for (const auto& error : capturingIterSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("capturing-closure iterator did not survive Sealer canonicalization");
    }

    // Iterator terminals with closure callbacks: for_each and fold must
    // accept Closure-typed callables (not just Function) in the canonical
    // CFG. A lambda callback's type is Closure, not Function.
    const std::string terminalClosureSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    values.iter().for_each(fn(x: &i32) -> unit {
        total = total + *x;
    });
    if total > 0 { return total; }
    return 0;
}
)luna";
    auto terminalClosureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        terminalClosureSource, "<terminal-closure>");
    if (!terminalClosureSnapshot.success())
        return fail("frontend rejected the terminal closure source");
    moon::LunaLowerer terminalClosureLowerer;
    auto terminalClosureModule = terminalClosureLowerer.lower(
        *terminalClosureSnapshot.program(),
        *terminalClosureSnapshot.symbolTable());
    if (!terminalClosureLowerer.errors().empty()) {
        for (const auto& diagnostic : terminalClosureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the terminal closure source");
    }
    moon::Sealer terminalClosureSealer;
    if (!terminalClosureSealer.sealFunctionBodies(*terminalClosureModule) ||
        !verifier.verify(*terminalClosureModule)) {
        for (const auto& error : terminalClosureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("for_each with closure callback did not survive Sealer canonicalization");
    }

    // fold with a closure reducer
    const std::string foldClosureSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let result = values.into_iter().fold(0, fn(acc: i32, x: i32) -> i32 {
        return acc + x;
    });
    if result > 0 { return result; }
    return 0;
}
)luna";
    auto foldClosureSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        foldClosureSource, "<fold-closure>");
    if (!foldClosureSnapshot.success())
        return fail("frontend rejected the fold closure source");
    moon::LunaLowerer foldClosureLowerer;
    auto foldClosureModule = foldClosureLowerer.lower(
        *foldClosureSnapshot.program(),
        *foldClosureSnapshot.symbolTable());
    if (!foldClosureLowerer.errors().empty()) {
        for (const auto& diagnostic : foldClosureLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the fold closure source");
    }
    moon::Sealer foldClosureSealer;
    if (!foldClosureSealer.sealFunctionBodies(*foldClosureModule) ||
        !verifier.verify(*foldClosureModule)) {
        for (const auto& error : foldClosureSealer.errors())
            std::cerr << error << '\n';
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("fold with closure reducer did not survive Sealer canonicalization");
    }

    // CompilerPipeline sealing gate: when LUNA_SEAL_CANONICAL=1 is set, the
    // production pipeline seals function bodies into canonical CFGs. Verify
    // that a simple program compiles and runs correctly under this gate.
    {
        const std::string pipelineSource = R"luna(
fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    for v in values { total = total + v; }
    if total > 0 { return total; }
    return 0;
}
)luna";
        luna::driver::CompilerPipeline pipeline;
        luna::driver::CompilerPipelineOptions options;
        setenv("LUNA_SEAL_CANONICAL", "1", 1);
        bool compiled = pipeline.compileSourceToMoonIR(
            pipelineSource, "<pipeline-seal>", options);
        unsetenv("LUNA_SEAL_CANONICAL");
        if (!compiled) {
            for (const auto& diag : pipeline.errors())
                std::cerr << diagnostic::render(diag) << '\n';
            return fail("pipeline sealing gate rejected a simple for-each program");
        }
        // The sealed module should have at least one function with a CFG.
        bool hasCgf = false;
        for (const auto& decl : pipeline.moonModule().declarations) {
            const auto* fn = dynamic_cast<const moon::FunctionDecl*>(decl.get());
            if (fn && fn->controlFlow) { hasCgf = true; break; }
        }
        if (!hasCgf)
            return fail("pipeline sealing gate did not produce a canonical CFG");
    }

    return 0;
}
