#include "core/TypeLayout.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/MoonIR.h"
#include "moonir/Verifier.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorTests(ControlFlowTestContext& context) {
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto& rangeIterator = context.rangeIterator;
    const auto rangeId = context.rangeId;
    const auto i32Id = context.i32Id;

    moon::Module protocolModule;
    protocolModule.name = "canonical.iterator";
    auto iteratorState = Type::makeStruct("IteratorState", {{"position", TyI32}},
                                          "canonical.iterator::IteratorState");
    auto iteratorReceiver = Type::makeReference(iteratorState, true);
    auto iteratorOption = Type::makeEnum("Option", {{"None", {}}, {"Some", {TyI32}}},
                                         "canonical.iterator::Option<i32>");
    auto iteratorNextType = Type::makeFunction(
        {iteratorReceiver}, iteratorOption,
        {{luna::ownership::Relation::MutableBorrow, luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned, luna::ownership::Usage::Copy});
    auto intoIteratorType = Type::makeFunction(
        {TyI32}, iteratorState, {{luna::ownership::Relation::Owned, luna::ownership::Usage::Copy}},
        {luna::ownership::Relation::Owned, luna::ownership::Usage::Affine});
    auto collected =
        Type::makeStruct("Collected", {{"total", TyI32}}, "canonical.iterator::Collected");
    auto collectBuilderReceiver = Type::makeReference(iteratorState, true);
    auto collectBeginType = Type::makeFunction(
        {}, iteratorState, {}, {luna::ownership::Relation::Owned, luna::ownership::Usage::Affine});
    auto collectPushType = Type::makeFunction(
        {collectBuilderReceiver, TyI32}, TyUnit,
        {{luna::ownership::Relation::MutableBorrow, luna::ownership::Usage::Copy},
         {luna::ownership::Relation::Owned, luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned, luna::ownership::Usage::Copy});
    auto collectFinishType =
        Type::makeFunction({iteratorState}, collected,
                           {{luna::ownership::Relation::Owned, luna::ownership::Usage::Affine}},
                           {luna::ownership::Relation::Owned, luna::ownership::Usage::Affine});
    const auto iteratorStateId = protocolModule.registerType(iteratorState);
    const auto iteratorOptionId = protocolModule.registerType(iteratorOption);
    const auto iteratorNextTypeId = protocolModule.registerType(iteratorNextType);
    const auto intoIteratorTypeId = protocolModule.registerType(intoIteratorType);
    const auto protocolI32Id = protocolModule.registerType(TyI32);
    const auto protocolRangeId = protocolModule.registerType(rangeIterator);
    const auto collectedId = protocolModule.registerType(collected);
    const auto collectBeginTypeId = protocolModule.registerType(collectBeginType);
    const auto collectPushTypeId = protocolModule.registerType(collectPushType);
    const auto collectFinishTypeId = protocolModule.registerType(collectFinishType);
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
    const auto addCollectDeclaration = [&](const std::string& name, const moon::TypeRef& type) {
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
    const auto* sealedNext = protocolModule.findDeclarationById("canonical.iterator::fn::next");
    if (!sealedNext) return fail("iterator protocol fixture lost its declaration row");
    const auto* sealedInto = protocolModule.findDeclarationById("canonical.iterator::fn::into");
    if (!sealedInto) return fail("IntoIterator fixture lost its declaration row");
    const auto* sealedCollectBegin =
        protocolModule.findDeclarationById("canonical.iterator::fn::collect_begin");
    const auto* sealedCollectPush =
        protocolModule.findDeclarationById("canonical.iterator::fn::collect_push");
    const auto* sealedCollectFinish =
        protocolModule.findDeclarationById("canonical.iterator::fn::collect_finish");
    if (!sealedCollectBegin || !sealedCollectPush || !sealedCollectFinish ||
        protocolRangeId != rangeId || protocolI32Id != i32Id)
        return fail("collect protocol fixture lost its frozen witness rows");

    auto protocolStructured = std::make_unique<moon::BlockStmt>();
    auto protocolLoop = std::make_unique<moon::ForStmt>();
    protocolLoop->varName = "item";
    protocolLoop->bindingUsage = luna::ownership::Usage::Copy;
    protocolLoop->elementType = protocolI32Id;
    protocolLoop->protocolNext = {sealedNext->symbolId, sealedNext->contractId};
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
    auto protocolCfg = cfgBuilder.build(std::move(protocolStructured), {iteratorParameter},
                                        moon::RegionKind::Function, protocolModule);
    const bool protocolVerified = protocolCfg && cfgVerifier.verify(*protocolCfg, protocolModule);
    if (!protocolVerified || protocolCfg->blocks.size() != 5 || protocolCfg->locals.size() != 2 ||
        protocolCfg->blocks[1].terminator.kind != moon::TerminatorKind::Switch ||
        protocolCfg->blocks[2].terminator.kind != moon::TerminatorKind::Jump ||
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
        protocolCases[1].bindings != std::vector<moon::LocalId>{moon::LocalId{1}})
        return fail("protocol for-loop lost its per-iteration pattern local");
    protocolCases[1].bindings.clear();
    if (cfgVerifier.verify(*protocolCfg, protocolModule))
        return fail("CFG verifier accepted a missing iterator item binding");

    auto intoStructured = std::make_unique<moon::BlockStmt>();
    auto intoLoop = std::make_unique<moon::ForStmt>();
    intoLoop->varName = "item";
    intoLoop->bindingUsage = luna::ownership::Usage::Copy;
    intoLoop->elementType = protocolI32Id;
    intoLoop->protocolNext = {sealedNext->symbolId, sealedNext->contractId};
    intoLoop->protocolIteratorType = iteratorStateId;
    intoLoop->protocolOptionType = iteratorOptionId;
    intoLoop->protocolNoneVariant = 0;
    intoLoop->protocolSomeVariant = 1;
    intoLoop->protocolInto = {sealedInto->symbolId, sealedInto->contractId};
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
    auto intoCfg = cfgBuilder.build(std::move(intoStructured), {sourceParameter},
                                    moon::RegionKind::Function, protocolModule);
    if (!intoCfg || !cfgVerifier.verify(*intoCfg, protocolModule) || intoCfg->blocks.size() != 6 ||
        intoCfg->locals.size() != 3 || intoCfg->cleanups.size() != 1 ||
        intoCfg->blocks[2].terminator.cases[0].edge.cleanups !=
            std::vector<moon::CleanupId>{moon::CleanupId{0}})
        return fail("IntoIterator state did not receive a canonical exit cleanup");
    intoCfg->blocks[2].terminator.cases[0].edge.cleanups.clear();
    if (cfgVerifier.verify(*intoCfg, protocolModule))
        return fail("CFG verifier accepted a leaked IntoIterator state");

    if (const int result = runIteratorRecipeTests(context)) return result;
    if (const int result = runIteratorTerminalTests(context)) return result;
    if (const int result = runIteratorCleanupTests(context)) return result;

    auto collectStructured = std::make_unique<moon::BlockStmt>();
    collectStructured->stmts.push_back(
        makeMaterializedRangeBinding(context, "collectPending", false));
    auto collectBinding = std::make_unique<moon::LetStmt>();
    collectBinding->name = "collected";
    collectBinding->type = collectedId;
    collectBinding->usage = luna::ownership::Usage::Affine;
    auto collectTerminal = makeIteratorTerminal(context, "collectPending", IteratorOp::Collect,
                                                collectedId, collectedId);
    collectTerminal->returnUsage = luna::ownership::Usage::Affine;
    collectTerminal->iteratorCollectTargetType = collectedId;
    collectTerminal->iteratorCollectBuilderType = iteratorStateId;
    collectTerminal->iteratorCollectBegin = {sealedCollectBegin->symbolId,
                                             sealedCollectBegin->contractId};
    collectTerminal->iteratorCollectPush = {sealedCollectPush->symbolId,
                                            sealedCollectPush->contractId};
    collectTerminal->iteratorCollectFinish = {sealedCollectFinish->symbolId,
                                              sealedCollectFinish->contractId};
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
    auto collectCfg = cfgBuilder.build(std::move(collectStructured), {}, moon::RegionKind::Function,
                                       protocolModule);
    moon::LocalId collectBuilderLocal;
    moon::LocalId collectResultLocal;
    moon::CallExpr* loweredPush = nullptr;
    moon::CallExpr* loweredFinish = nullptr;
    moon::LetStmt* loweredUserCollect = nullptr;
    bool retainedCollectTerminal = false;
    if (collectCfg)
        for (auto& block : collectCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get())) {
                    if (declaration->name.rfind("$terminal.collect.builder.", 0) == 0)
                        collectBuilderLocal = declaration->local;
                    else if (declaration->name.rfind("$terminal.collect.result.", 0) == 0) {
                        collectResultLocal = declaration->local;
                        loweredFinish =
                            dynamic_cast<moon::CallExpr*>(declaration->initializer.get());
                    } else if (declaration->name == "collected") {
                        loweredUserCollect = declaration;
                    }
                } else if (auto* expression = dynamic_cast<moon::ExprStmt*>(operation.get())) {
                    auto* call = dynamic_cast<moon::CallExpr*>(expression->expr.get());
                    retainedCollectTerminal = retainedCollectTerminal ||
                                              (call && call->iteratorOp == IteratorOp::Collect);
                    if (call &&
                        call->calleeRef == moon::DeclarationRef{sealedCollectPush->symbolId,
                                                                sealedCollectPush->contractId})
                        loweredPush = call;
                }
            }
    auto* builderBorrow = loweredPush && loweredPush->args.size() == 2
                              ? dynamic_cast<moon::BorrowExpr*>(loweredPush->args[0].get())
                              : nullptr;
    auto* borrowedBuilder =
        builderBorrow ? dynamic_cast<moon::IdentifierExpr*>(builderBorrow->operand.get()) : nullptr;
    auto* finishMove = loweredFinish && loweredFinish->args.size() == 1
                           ? dynamic_cast<moon::MoveExpr*>(loweredFinish->args[0].get())
                           : nullptr;
    auto* finishedBuilder =
        finishMove ? dynamic_cast<moon::IdentifierExpr*>(finishMove->operand.get()) : nullptr;
    auto* resultMove = loweredUserCollect
                           ? dynamic_cast<moon::MoveExpr*>(loweredUserCollect->initializer.get())
                           : nullptr;
    auto* transferredResult =
        resultMove ? dynamic_cast<moon::IdentifierExpr*>(resultMove->operand.get()) : nullptr;
    if (!collectCfg || !cfgVerifier.verify(*collectCfg, protocolModule) ||
        collectBuilderLocal.empty() || collectResultLocal.empty() || !builderBorrow ||
        !builderBorrow->isMutable || !borrowedBuilder ||
        borrowedBuilder->local != collectBuilderLocal || !finishMove || !finishedBuilder ||
        finishedBuilder->local != collectBuilderLocal || !resultMove || !transferredResult ||
        transferredResult->local != collectResultLocal || retainedCollectTerminal ||
        collectCfg->cleanups.size() != 3) {
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
                      << " user=" << (loweredUserCollect != nullptr) << '\n';
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
    auto directCollect =
        makeDirectRangeTerminal(context, IteratorOp::Collect, collectedId, collectedId, true);
    directCollect->returnUsage = luna::ownership::Usage::Affine;
    directCollect->iteratorCollectTargetType = collectedId;
    directCollect->iteratorCollectBuilderType = iteratorStateId;
    directCollect->iteratorCollectBegin = {sealedCollectBegin->symbolId,
                                           sealedCollectBegin->contractId};
    directCollect->iteratorCollectPush = {sealedCollectPush->symbolId,
                                          sealedCollectPush->contractId};
    directCollect->iteratorCollectFinish = {sealedCollectFinish->symbolId,
                                            sealedCollectFinish->contractId};
    directCollectBinding->initializer = std::move(directCollect);
    directCollectStructured->stmts.push_back(std::move(directCollectBinding));
    auto directCollectCleanup = std::make_unique<moon::FreeStmt>();
    directCollectCleanup->isImplicit = true;
    directCollectCleanup->action = cleanupActionForType(collected);
    auto directCollectedIdentifier = std::make_unique<moon::IdentifierExpr>();
    directCollectedIdentifier->name = "directCollected";
    directCollectedIdentifier->type = collectedId;
    directCollectCleanup->operand = std::move(directCollectedIdentifier);
    directCollectStructured->stmts.push_back(std::move(directCollectCleanup));
    auto directCollectCfg = cfgBuilder.build(std::move(directCollectStructured), {},
                                             moon::RegionKind::Function, protocolModule);
    bool directCollectCursor = false;
    bool directCollectBuilder = false;
    bool directCollectResult = false;
    bool directCollectRetainedIterator = false;
    if (directCollectCfg)
        for (const auto& local : directCollectCfg->locals) {
            directCollectCursor =
                directCollectCursor || local.name.rfind("$terminal.cursor.", 0) == 0;
            directCollectBuilder =
                directCollectBuilder || local.name.rfind("$terminal.collect.builder.", 0) == 0;
            directCollectResult =
                directCollectResult || local.name.rfind("$terminal.collect.result.", 0) == 0;
            const auto* type = protocolModule.findType(local.type);
            directCollectRetainedIterator =
                directCollectRetainedIterator || (type && type->kind == TypeKind::Iterator);
        }
    if (!directCollectCfg || !cfgVerifier.verify(*directCollectCfg, protocolModule) ||
        !directCollectCursor || !directCollectBuilder || !directCollectResult ||
        directCollectRetainedIterator || directCollectCfg->cleanups.size() != 3)
        return fail("direct collect did not erase to affine builder CFG state");

    return 0;
}

} // namespace canonical_test
