#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "core/TypeLayout.h"
#include "driver/MoonGeneration.h"
#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "selector/Selector.h"
#include "sema/SemanticAnalysisSupport.h"
#include "sema/SymbolTable.h"
#include "tooling/AnalysisSnapshot.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/TargetSelect.h>

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
#include "moonir_canonical_test_support.h"

using namespace canonical_test;

int main() {
    FunctionDecl unaryRoute;
    unaryRoute.name = "route";
    unaryRoute.packageId = "identity.test";
    unaryRoute.generatedSymbolName = "route__single_linkage";
    Decl::MetadataAttachment unaryRevision;
    unaryRevision.schemaName = "revision";
    unaryRevision.arguments.push_back(std::make_unique<IntLiteralExpr>(3));
    unaryRoute.metadata.push_back(std::move(unaryRevision));
    Param routeValue;
    routeValue.name = "value";
    routeValue.type = std::make_unique<NamedTypeAST>("i32");
    unaryRoute.params.push_back(std::move(routeValue));
    unaryRoute.returnType = std::make_unique<NamedTypeAST>("i32");
    const auto unaryRouteIdentity =
        functionDeclarationIdentity(nullptr, &unaryRoute);
    unaryRoute.generatedSymbolName = "route__overload_family_linkage";
    if (functionDeclarationIdentity(nullptr, &unaryRoute) !=
        unaryRouteIdentity)
        return fail("function identity changed with generated linkage");

    FunctionDecl nullaryRoute;
    nullaryRoute.name = "route";
    nullaryRoute.packageId = "identity.test";
    Decl::MetadataAttachment nullaryRevision;
    nullaryRevision.schemaName = "revision";
    nullaryRevision.arguments.push_back(std::make_unique<IntLiteralExpr>(3));
    nullaryRoute.metadata.push_back(std::move(nullaryRevision));
    nullaryRoute.returnType = std::make_unique<NamedTypeAST>("i32");
    if (functionDeclarationIdentity(nullptr, &nullaryRoute) ==
        unaryRouteIdentity)
        return fail("same-metadata overload signatures shared an identity");

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
    const auto* canonicalResultRecord = module.findType(resultI32BoolId);
    if (!canonicalResultRecord ||
        canonicalResultRecord->identityMode !=
            luna::types::IdentityMode::Nominal ||
        canonicalResultRecord->nominalDeclarationId !=
            luna::sysmeta::ResultTypeId ||
        canonicalResultRecord->typeArgumentIds.size() != 2 ||
        canonicalResultRecord->layoutAbiVersion !=
            luna::layout::InlineAdtAbiVersion)
        return fail(
            "Result lost its canonical Core identity or inline ADT layout");
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
    moon::Module leakedQueryModule;
    leakedQueryModule.name = "canonical.query-leak";
    leakedQueryModule.registerType(Type::makeSymbolSet(lambdaType));
    leakedQueryModule.sealTypeTable();
    moon::Verifier leakedQueryVerifier;
    if (leakedQueryVerifier.verify(leakedQueryModule))
        return fail("MoonIR verifier accepted a compiler-only symbol_set type");
    const bool rejectedQueryType = std::any_of(
        leakedQueryVerifier.errors().begin(),
        leakedQueryVerifier.errors().end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find(
                "was not erased before MoonIR") != std::string::npos;
        });
    if (!rejectedQueryType)
        return fail("MoonIR verifier did not diagnose leaked symbol_set type");
    moon::Module leakedOptionalModule;
    leakedOptionalModule.name = "canonical.optional-leak";
    leakedOptionalModule.registerType(Type::makeCompileTimeOption(
        Type::makeDeclarationRef(lambdaType)));
    leakedOptionalModule.sealTypeTable();
    moon::Verifier leakedOptionalVerifier;
    if (leakedOptionalVerifier.verify(leakedOptionalModule))
        return fail("MoonIR verifier accepted a compiler-only Option type");
    const bool rejectedOptionalType = std::any_of(
        leakedOptionalVerifier.errors().begin(),
        leakedOptionalVerifier.errors().end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find(
                "compiler-only Option type") != std::string::npos;
        });
    if (!rejectedOptionalType)
        return fail("MoonIR verifier did not diagnose leaked Option type");
    cfg.blocks.front().id = moon::BlockId{1};
    if (cfgVerifier.verify(cfg, module))
        return fail("CFG verifier accepted a forged canonical block index");
    cfg.blocks.front().id = cfg.entry;

    auto literalStatement = std::make_unique<moon::ExprStmt>();
    auto literalValue = std::make_unique<moon::IntLiteralExpr>();
    auto* literal = literalValue.get();
    literal->value = 7;
    literalStatement->expr = std::move(literalValue);
    cfg.blocks.front().operations.push_back(std::move(literalStatement));
    if (cfgVerifier.verify(cfg, module))
        return fail("CFG verifier accepted an integer literal without a TypeRef");
    literal->type = boolId;
    if (cfgVerifier.verify(cfg, module))
        return fail("CFG verifier accepted an integer literal with boolean type");
    literal->type = i32Id;
    if (!cfgVerifier.verify(cfg, module))
        return fail("CFG verifier rejected a correctly typed integer literal");
    cfg.blocks.front().operations.clear();

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
        moon::CleanupId{0}, moon::CleanupId{1}};
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

    moon::ControlFlowBuilder cfgBuilder;
    ControlFlowTestContext controlFlowContext{
        module, cfgVerifier, cfgBuilder, rangeIterator, ownedProduct,
        shortId, rangeId, sharedId, sliceId, sharedSliceId,
        mutableSliceId, productId, ownedProductId, inlineProductId,
        i32Id, usizeId, stringId, guardedArrayId, boolId, unitId,
        resultI32BoolId, lambdaTypeId, closureTypeId, predicateTypeId,
        reducerTypeId, affineReducerTypeId, linearReducerTypeId,
        affineValueProducerTypeId, actionTypeId, unitConsumerTypeId,
        unitOrderedConsumerTypeId, moveMapTypeId, affinePredicateTypeId,
        affineIdentityTypeId, moveMapIteratorId, choiceId};
    trace("control-flow tests");
    if (const int result = runControlFlowTests(controlFlowContext))
        return result;


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

    trace("sealing tests");
    if (const int result = runSealingTests(
            cfgBuilder, cfgVerifier, module, reverse, shortId, productId))
        return result;
    trace("registered pipeline tests");
    if (const int result = runRegisteredTests()) return result;

    return 0;
}
