#include "BodyAnalyzer.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include "BodyAnalyzerInternal.h"

TypePtr BodyAnalyzer::analyzeExpr(Expr* expr) {
    mContext.setDiagnosticLocation(expr);
    if (auto* literal = dynamic_cast<IntLiteralExpr*>(expr)) {
        if (!literal->inferredType) {
            literal->inferredType = mContext.mConstraints.fresh();
            mContext.requireInteger(literal->inferredType, "integer literal");
        }
        return mContext.resolved(literal->inferredType);
    }
    if (dynamic_cast<FloatLiteralExpr*>(expr)) return TyF64;
    if (dynamic_cast<StringLiteralExpr*>(expr)) return TyString;
    if (dynamic_cast<BoolLiteralExpr*>(expr)) return TyBool;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        const std::string declarationKey = mContext.sourceDeclarationKey(id->name);
        auto family = mContext.mFunctionFamilies.find(declarationKey);
        if (family != mContext.mFunctionFamilies.end() && family->second.size() > 1) {
            mContext.error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", id->line, id->col);
            return TyUnknown;
        }
        auto* sym = mContext.lookupSymbol(id->name);
        if (!sym) {
            mContext.error("undefined name '" + id->name + "'", id->line, id->col);
            return TyUnknown;
        }
        if (!mCaptureFrames.empty() &&
            sym->kind == SymbolKind::Variable) {
            auto& frame = mCaptureFrames.back();
            if (mContext.mSymTable.lookupDepth(id->name) <
                frame.lambdaScopeDepth) {
                if (std::find(frame.captures.begin(),
                              frame.captures.end(),
                              id->name) == frame.captures.end())
                    frame.captures.push_back(id->name);
            }
        }
        if (sym->kind == SymbolKind::Function) {
            if (family != mContext.mFunctionFamilies.end() && family->second.size() == 1) {
                const auto* declaration = family->second.front();
                id->resolvedSymbolName =
                    declaration->generatedSymbolName.empty()
                        ? declaration->name
                        : declaration->generatedSymbolName;
                mContext.recordDeclarationReference(id, id->name.size(),
                                                   declaration);
            }
            return Type::makeFunction(sym->paramTypes,
                                      sym->returnType ? sym->returnType : TyUnit,
                                      sym->paramContracts,
                                      {luna::ownership::Relation::Owned,
                                       sym->returnUsage});
        }
        return sym->type ? mContext.resolved(sym->type) : TyUnknown;
    }
    if (auto* selection = dynamic_cast<SelectExpr*>(expr))
        return analyzeSelect(selection);
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr))
        return analyzeRecordLiteralExpr(record);
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        TypePtr lhsType = analyzeExpr(bin->lhs.get());
        TypePtr rhsType = analyzeExpr(bin->rhs.get());
        switch (bin->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                mContext.requireNumeric(lhsType, "left operand of arithmetic expression");
                mContext.requireNumeric(rhsType, "right operand of arithmetic expression");
                mContext.constrain(lhsType, rhsType, "arithmetic operands");
                return mContext.resolved(lhsType);
            case TokenKind::Ampersand:
            case TokenKind::BitOr:
            case TokenKind::BitXor:
                mContext.requireInteger(lhsType, "left operand of bitwise expression");
                mContext.requireInteger(rhsType, "right operand of bitwise expression");
                if ((mContext.resolved(lhsType)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(lhsType))) &&
                    (mContext.resolved(rhsType)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(rhsType))))
                    mContext.constrain(lhsType, rhsType, "bitwise operands");
                return mContext.resolved(lhsType);
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                mContext.requireInteger(lhsType, "left operand of shift expression");
                mContext.requireInteger(rhsType, "shift count");
                return mContext.resolved(lhsType);
            case TokenKind::EqEq:
            case TokenKind::Neq:
                mContext.constrain(lhsType, rhsType, "equality operands");
                return TyBool;
            case TokenKind::Lt:
            case TokenKind::LtEq:
            case TokenKind::Gt:
            case TokenKind::GtEq:
                mContext.requireNumeric(lhsType, "left operand of comparison expression");
                mContext.requireNumeric(rhsType, "right operand of comparison expression");
                mContext.constrain(lhsType, rhsType, "comparison operands");
                return TyBool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                mContext.requireBool(lhsType, "left operand of logical expression");
                mContext.requireBool(rhsType, "right operand of logical expression");
                return TyBool;
            default: return TyUnknown;
        }
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        TypePtr opType = analyzeExpr(un->operand.get());
        switch (un->op) {
            case TokenKind::Minus:
                mContext.requireNumeric(opType, "unary '-' operand");
                return opType;
            case TokenKind::Not:
                mContext.requireBool(opType, "'!' operand");
                return TyBool;
            case TokenKind::Tilde:
                mContext.requireInteger(opType, "'~' operand");
                return opType;
            case TokenKind::Star: {
                auto resolvedOp = mContext.resolved(opType);
                if (resolvedOp->kind == TypeKind::InferenceVar) {
                    auto inner = mContext.mConstraints.fresh();
                    mContext.constrain(opType, Type::makeReference(inner), "dereference operand");
                    return inner;
                }
                if (resolvedOp->kind == TypeKind::Reference) return resolvedOp->inner;
                mContext.error("Cannot dereference non-reference type");
                return TyUnknown;
            }
            default: return TyUnknown;
        }
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr))
        return analyzeVariantConstructExpr(variant);
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return analyzeLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr)) return analyzeCall(call);
    if (auto* fa = dynamic_cast<FieldAccessExpr*>(expr)) {
        TypePtr objectType = mContext.resolved(analyzeExpr(fa->object.get()));
        if (objectType->kind == TypeKind::Reference && objectType->inner)
            objectType = mContext.resolved(objectType->inner);
        if (objectType->kind != TypeKind::Struct &&
            objectType->kind != TypeKind::Record &&
            objectType->kind != TypeKind::Metadata) {
            mContext.error("Field access requires a product type, got " + objectType->toString());
            return TyUnknown;
        }
        for (auto& field : objectType->fields) {
            if (field.name == fa->field) {
                fa->resultType = mContext.resolved(field.type);
                if (!objectType->declarationLinkageName.empty())
                    mContext.recordResolvedReference(
                        fa->sourcePath, fa->line, fa->col, fa->field.size(),
                        objectType->declarationLinkageName + "::field::" +
                            fa->field);
                return fa->resultType;
            }
        }
        mContext.error("Type '" + objectType->toString() + "' has no field '" + fa->field + "'");
        return TyUnknown;
    }
    if (auto* propagation = dynamic_cast<TryExpr*>(expr))
        return analyzeTryExpr(propagation);
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr)) {
        if (mContext.mInKernel) {
            mContext.error("kernel body may not allocate heap memory with `new`; allocate device memory on the host and pass a borrowed device_buffer parameter",
                  ha->line, ha->col);
            return TyUnknown;
        }
        ha->allocatedType = ha->allocatedTypeAST
            ? mContext.resolveTypeAST(ha->allocatedTypeAST.get(), {}) : TyUnknown;
        if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
            TypeVec argumentTypes;
            for (auto& arg : initCall->args)
                argumentTypes.push_back(analyzeExpr(arg.get()));
            if (ha->allocatedType->kind == TypeKind::Struct ||
                ha->allocatedType->kind == TypeKind::Record) {
                if (initCall->args.size() != ha->allocatedType->fields.size()) {
                    mContext.error("Constructor for '" + ha->allocatedType->toString() +
                          "' expects " + std::to_string(ha->allocatedType->fields.size()) +
                          " field values");
                } else {
                    for (size_t i = 0; i < initCall->args.size(); ++i)
                        mContext.constrain(argumentTypes[i],
                                  ha->allocatedType->fields[i].type,
                                  "field '" + ha->allocatedType->fields[i].name + "' initializer");
                }
            } else if (ha->allocatedType->kind == TypeKind::I32 ||
                       ha->allocatedType->kind == TypeKind::I64 ||
                       ha->allocatedType->kind == TypeKind::F32 ||
                       ha->allocatedType->kind == TypeKind::F64 ||
                       ha->allocatedType->kind == TypeKind::Bool ||
                       ha->allocatedType->kind == TypeKind::String) {
                if (argumentTypes.size() != 1)
                    mContext.error("Primitive allocation requires exactly one initializer");
                else
                    mContext.constrain(argumentTypes[0], ha->allocatedType,
                              "primitive allocation initializer");
            }
        }
        ha->resultType = ha->allocatedType;
        return ha->resultType;
    }
    if (auto* mv = dynamic_cast<MoveExpr*>(expr)) {
        return analyzeExpr(mv->operand.get());
    }
    if (auto* bw = dynamic_cast<BorrowExpr*>(expr)) {
        TypePtr inner = analyzeExpr(bw->operand.get());
        return Type::makeReference(inner, bw->isMutable);
    }
    if (auto* dr = dynamic_cast<DerefExpr*>(expr)) {
        TypePtr op = mContext.resolved(analyzeExpr(dr->operand.get()));
        if (op->kind == TypeKind::InferenceVar) {
            auto inner = mContext.mConstraints.fresh();
            mContext.constrain(
                op, Type::makeReference(inner), "dereference operand");
            dr->resultType = inner;
            return inner;
        }
        if ((op->kind == TypeKind::Reference ||
             op->kind == TypeKind::RawPointer) && op->inner) {
            dr->resultType = mContext.resolved(op->inner);
            return dr->resultType;
        }
        mContext.error("Cannot dereference non-reference or non-raw-pointer type");
        return TyUnknown;
    }
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr)) {
        TypePtr op = analyzeExpr(ad->operand.get());
        return Type::makeReference(op, ad->isMutable);
    }
    if (auto* le = dynamic_cast<LambdaExpr*>(expr))
        return analyzeLambdaExpr(le);
    if (auto* as = dynamic_cast<AssignExpr*>(expr)) {
        TypePtr rhs = analyzeExpr(as->rhs.get());
        TypePtr lhs = analyzeExpr(as->lhs.get());
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            if (auto* symbol = mContext.mSymTable.lookup(id->name); symbol && symbol->isConst)
                mContext.error("cannot assign to const binding '" + id->name + "'", id->line, id->col);
        }
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(index->object.get())) {
                if (auto* symbol = mContext.mSymTable.lookup(id->name); symbol && symbol->isConst)
                    mContext.error("cannot assign through index of const array binding '" + id->name + "'",
                          id->line, id->col);
            }
        }
        bool operandsValid = true;
        if (as->op != TokenKind::Eq) {
            switch (as->op) {
                case TokenKind::AndEq:
                case TokenKind::OrEq:
                case TokenKind::XorEq:
                case TokenKind::ShiftLeftEq:
                case TokenKind::ShiftRightEq:
                    mContext.requireInteger(lhs, "left operand of compound bitwise assignment");
                    mContext.requireInteger(rhs, "right operand of compound bitwise assignment");
                    operandsValid =
                        (mContext.resolved(lhs)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(lhs))) &&
                        (mContext.resolved(rhs)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(rhs)));
                    break;
                default:
                    mContext.requireNumeric(lhs, "left operand of compound arithmetic assignment");
                    mContext.requireNumeric(rhs, "right operand of compound arithmetic assignment");
                    break;
            }
        }
        if (operandsValid) mContext.constrain(rhs, lhs, "assignment");
        return rhs;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
        TypePtr object = mContext.resolved(analyzeExpr(ix->object.get()));
        TypePtr index = mContext.resolved(analyzeExpr(ix->index.get()));
        mContext.requireInteger(index, "array index");
        if (index->kind != TypeKind::InferenceVar && !isIntegerType(index))
            mContext.error("array index must be an integer, got " + index->toString(), ix->line, ix->col);
        if (object->kind != TypeKind::Array && object->kind != TypeKind::Slice) {
            mContext.error("cannot index " + object->toString() + "; only array<T, N> and slice<T> support safe indexing",
                  ix->line, ix->col);
            return TyUnknown;
        }
        if (object->kind == TypeKind::Array) if (auto* literal = dynamic_cast<IntLiteralExpr*>(ix->index.get());
            literal && (literal->value < 0 || static_cast<uint64_t>(literal->value) >= object->arrayLength))
            mContext.error("array index " + std::to_string(literal->value) + " is outside array length " +
                  std::to_string(object->arrayLength), ix->line, ix->col);
        return object->inner;
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        if (array->elements.empty()) {
            mContext.error("cannot infer the element type of an empty array", array->line, array->col);
            return TyUnknown;
        }
        TypePtr element = analyzeExpr(array->elements.front().get());
        for (size_t i = 1; i < array->elements.size(); ++i)
            mContext.constrain(analyzeExpr(array->elements[i].get()), element, "array element");
        array->elementType = mContext.resolved(element);
        return Type::makeArray(array->elementType, array->elements.size());
    }
    if (auto* ie = dynamic_cast<IfExpr*>(expr)) {
        mContext.requireBool(analyzeExpr(ie->cond.get()), "if-expression condition");
        auto branchType = [this](Expr* branch) {
            if (auto* be = dynamic_cast<BlockExpr*>(branch)) {
                analyzeBlock(be->block.get(), nullptr);
                return TyUnit;
            }
            return analyzeExpr(branch);
        };
        TypePtr thenType = branchType(ie->thenExpr.get());
        TypePtr elseType = branchType(ie->elseExpr.get());
        if (mContext.resolved(thenType)->kind == TypeKind::Never) return elseType;
        if (mContext.resolved(elseType)->kind == TypeKind::Never) return thenType;
        mContext.constrain(thenType, elseType, "if-expression branches");
        return thenType;
    }
    return TyUnknown;
}


TypePtr BodyAnalyzer::analyzeSelect(SelectExpr* selection) {
    const std::string targetKey =
        mContext.sourceDeclarationKey(selection->targetName);
    const std::string selectorKey =
        mContext.sourceDeclarationKey(selection->selectorName);
    auto targetFamily = mContext.mFunctionFamilies.find(targetKey);
    if (targetFamily == mContext.mFunctionFamilies.end() ||
        targetFamily->second.empty()) {
        mContext.error("unknown declaration family '" + selection->targetName +
                           "'",
                       selection->line, selection->col);
        return TyUnknown;
    }
    auto selectorFamily = mContext.mFunctionFamilies.find(selectorKey);
    if (selectorFamily == mContext.mFunctionFamilies.end() ||
        selectorFamily->second.size() != 1) {
        mContext.error("selector function '" + selection->selectorName +
                           "' must resolve to exactly one declaration",
                       selection->line, selection->col);
        return TyUnknown;
    }

    auto* selectorFunction = selectorFamily->second.front();
    selection->resolvedSelectorDeclarationId =
        functionDeclarationIdentity(mContext.mProgram, selectorFunction);
    selection->resolvedFamilyId = nominalDeclarationIdentity(
        mContext.mProgram, "fn", targetFamily->second.front()->name,
        targetFamily->second.front());
    if (selectorFunction->params.empty() ||
        mContext.resolved(selectorFunction->params.front().inferredType)->kind !=
            TypeKind::DeclarationView) {
        mContext.error("selector function '" + selection->selectorName +
                           "' must declare declaration_view as its first parameter",
                       selection->line, selection->col);
        return TyUnknown;
    }
    if (selection->selectorArgs.size() + 1 !=
        selectorFunction->params.size()) {
        mContext.error("selector function '" + selection->selectorName +
                           "' expects " +
                           std::to_string(selectorFunction->params.size() - 1) +
                           " explicit arguments",
                       selection->line, selection->col);
        return TyUnknown;
    }
    if (mContext.resolved(selectorFunction->inferredReturnType)->kind !=
        TypeKind::DeclarationRef) {
        mContext.error("selector function '" + selection->selectorName +
                           "' must return declaration_ref",
                       selection->line, selection->col);
        return TyUnknown;
    }

    std::vector<ConstValue> selectorArguments;
    for (size_t index = 0; index < selection->selectorArgs.size(); ++index) {
        mContext.constrain(analyzeExpr(selection->selectorArgs[index].get()),
                           selectorFunction->params[index + 1].inferredType,
                           "selector argument " + std::to_string(index + 1));
        auto value =
            mContext.evaluateConstExpr(selection->selectorArgs[index].get());
        if (!value) {
            mContext.error("static selector argument " +
                               std::to_string(index + 1) +
                               " is not a compile-time value",
                           selection->line, selection->col);
            return TyUnknown;
        }
        selectorArguments.push_back(*value);
    }

    TypePtr callableType;
    for (auto* candidate : targetFamily->second) {
        TypeVec parameters;
        std::vector<luna::ownership::Contract> contracts;
        for (const auto& parameter : candidate->params) {
            parameters.push_back(mContext.resolved(parameter.inferredType));
            contracts.push_back({parameter.relation, parameter.usage});
        }
        auto candidateType = Type::makeFunction(
            std::move(parameters),
            mContext.resolved(candidate->inferredReturnType),
            std::move(contracts),
            {luna::ownership::Relation::Owned, candidate->returnUsage});
        if (!callableType) {
            callableType = candidateType;
        } else if (!luna::types::sameType(callableType, candidateType)) {
            mContext.error("declaration family '" + selection->targetName +
                               "' contains incompatible callable signatures",
                           selection->line, selection->col);
            return TyUnknown;
        }
    }

    if (!mContext.mSymbolCatalog || !mContext.mSymbolCatalog->valid()) {
        const std::string reason = mContext.mSymbolCatalog
            ? mContext.mSymbolCatalog->error()
            : "catalog snapshot is unavailable";
        mContext.error("selector catalog is invalid: " + reason,
                       selection->line, selection->col);
        return TyUnknown;
    }

    luna::selector::SymbolQuery query;
    query.phase = luna::selector::QueryPhase::CompileTime;
    query.kind = luna::selector::CatalogSymbolKind::Function;
    query.familyId =
        luna::identity::symbolIdFromCanonical(selection->resolvedFamilyId);
    query.typeId = luna::types::typeId(callableType);
    auto symbolSet = mContext.mSymbolCatalog->query(query);
    if (!symbolSet.valid()) {
        mContext.error("selector catalog for family '" +
                           selection->targetName + "' is invalid: " +
                           symbolSet.error(),
                       selection->line, selection->col);
        return TyUnknown;
    }
    if (symbolSet.size() != targetFamily->second.size()) {
        mContext.error(
            "declaration family '" + selection->targetName +
                "' does not have a complete stable signature projection in "
                "the compile-time Symbol Catalog; use explicit resolved "
                "parameter and return types for static selection",
            selection->line, selection->col);
        return TyUnknown;
    }

    std::string evaluationFailure;
    auto selectedId = mContext.evaluateSelectorFunction(
        selectorFunction, symbolSet, selectorArguments, evaluationFailure);
    std::vector<luna::identity::SymbolId> selectedSymbols;
    if (selectedId)
        selectedSymbols.push_back(
            luna::identity::symbolIdFromCanonical(*selectedId));
    auto result = symbolSet.select(selectedSymbols).one();
    if (!selectedId || !result.oneSucceeded()) {
        const std::string reason = !evaluationFailure.empty()
            ? evaluationFailure
            : result.message;
        mContext.error("selector '" + selection->selectorName +
                           "' failed for family '" + selection->targetName +
                           "': " + reason,
                       selection->line, selection->col);
        return TyUnknown;
    }

    selection->resolvedDeclarationId = result.selected->declarationId;
    selection->resolvedSymbolId = result.selected->symbolId;
    selection->resolvedContractId = result.selected->contractId;
    selection->resolvedSymbolName = result.selected->symbolName;
    selection->selectedType = callableType;
    selectorFunction->isSelector = true;
    return callableType;
}
