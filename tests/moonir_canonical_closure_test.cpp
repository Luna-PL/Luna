#include "moonir/MoonIR.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Verifier.h"
#include "core/TypeLayout.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runReferenceAndClosureTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto shortId = context.shortId;
    const auto rangeId = context.rangeId;
    const auto sharedId = context.sharedId;
    const auto sliceId = context.sliceId;
    const auto sharedSliceId = context.sharedSliceId;
    const auto mutableSliceId = context.mutableSliceId;
    const auto i32Id = context.i32Id;
    const auto usizeId = context.usizeId;
    const auto stringId = context.stringId;
    const auto boolId = context.boolId;
    const auto lambdaTypeId = context.lambdaTypeId;
    const auto closureTypeId = context.closureTypeId;
    const auto predicateTypeId = context.predicateTypeId;
    const auto moveMapTypeId = context.moveMapTypeId;
    const auto affinePredicateTypeId = context.affinePredicateTypeId;
    const auto affineIdentityTypeId = context.affineIdentityTypeId;
    const auto moveMapIteratorId = context.moveMapIteratorId;

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


    return 0;
}

} // namespace canonical_test
