#include "moonir/MoonIR.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Verifier.h"

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
                           cfg.rootScope, cfg.entry, {}, {cfg.entry}, {}});
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

    moon::Module module;
    module.name = "canonical.test";
    const auto shortId = module.registerType(shortIterator);
    const auto longId = module.registerType(longIterator);
    const auto rangeId = module.registerType(rangeIterator);
    const auto sharedId = module.registerType(sharedIterator);
    if (shortId == longId)
        return fail("backend-significant iterator sources collapsed to one TypeId");

    auto product = Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot");
    const auto productId = module.registerType(product);
    const auto i32Id = module.registerType(TyI32);
    const auto stringId = module.registerType(TyString);
    const auto boolId = module.registerType(TyBool);
    auto lambdaType = Type::makeFunction(
        {TyI32}, TyI32,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto lambdaTypeId = module.registerType(lambdaType);
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
        {moon::BlockId{0}, moon::BlockId{2}}, {}});
    cleanupCfg.regions.push_back({moon::RegionId{1}, moon::RegionId{0},
        moon::RegionKind::Lexical, moon::ScopeId{1}, moon::BlockId{1},
        moon::BlockId{2}, {moon::BlockId{1}}, {}});
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
        luna::ownership::CleanupAction::Deallocate});
    if (!cfgVerifier.verify(cleanupCfg, module))
        return fail("CFG verifier rejected a canonical scope-exit cleanup edge");
    cleanupCfg.blocks[1].terminator.primary.cleanups.clear();
    if (cfgVerifier.verify(cleanupCfg, module))
        return fail("CFG verifier accepted an omitted scope-exit cleanup");

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
    const auto iteratorStateId = protocolModule.registerType(iteratorState);
    const auto iteratorOptionId = protocolModule.registerType(iteratorOption);
    const auto iteratorNextTypeId = protocolModule.registerType(iteratorNextType);
    const auto intoIteratorTypeId = protocolModule.registerType(intoIteratorType);
    const auto protocolI32Id = protocolModule.registerType(TyI32);
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
    protocolModule.sealTypeTable();
    const auto* sealedNext = protocolModule.findDeclarationById(
        "canonical.iterator::fn::next");
    if (!sealedNext)
        return fail("iterator protocol fixture lost its declaration row");
    const auto* sealedInto = protocolModule.findDeclarationById(
        "canonical.iterator::fn::into");
    if (!sealedInto)
        return fail("IntoIterator fixture lost its declaration row");

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

    const auto* shortIteratorType = module.findType(shortId);
    const auto* sharedIteratorType = module.findType(sharedId);
    if (!shortIteratorType || shortIteratorType->typeArgumentIds.empty() ||
        !sharedIteratorType || sharedIteratorType->innerTypeId.empty())
        return fail("array iterator fixtures lost their frozen type witnesses");
    const auto arrayId = shortIteratorType->typeArgumentIds.front();
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
    moon::Param arrayParameter;
    arrayParameter.name = "values";
    arrayParameter.type = arrayId;
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
    reverse.registerType(rangeIterator);
    reverse.registerType(Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot"));
    reverse.registerType(TyString);
    reverse.registerType(TyBool);
    reverse.registerType(lambdaType);
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
    return 0;
}
