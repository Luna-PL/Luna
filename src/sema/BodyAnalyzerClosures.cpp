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

TypePtr BodyAnalyzer::analyzeTryExpr(TryExpr* propagation) {
        if (mContext.mCurrentFragmentDecl) {
            mContext.error("`?` may not propagate across a fragment/slot boundary; "
                  "handle the Result explicitly inside the fragment",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr result = analyzeExpr(propagation->operand.get());
        mContext.mConstraints.defaultNumeric(result);
        result = mContext.resolved(result);
        if (result->kind != TypeKind::Result ||
            result->typeArgs.size() != 2) {
            mContext.error("`?` requires Result<T, E>, got " + result->toString(),
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr functionResult = mContext.resolved(mContext.mCurrentReturnType);
        if (!mContext.mInFunction || functionResult->kind != TypeKind::Result ||
            functionResult->typeArgs.size() != 2) {
            mContext.error("`?` requires the enclosing function to return Result",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr sourceError = mContext.resolved(result->typeArgs[1]);
        TypePtr targetError = mContext.resolved(functionResult->typeArgs[1]);
        if (!luna::types::sameType(sourceError, targetError)) {
            auto targetConversions =
                mContext.mFromConversions.find(mContext.typeIdentity(targetError));
            auto conversion = targetConversions == mContext.mFromConversions.end()
                ? std::unordered_map<std::string, FromConversion>::const_iterator{}
                : targetConversions->second.find(mContext.typeIdentity(sourceError));
            if (targetConversions == mContext.mFromConversions.end() ||
                conversion == targetConversions->second.end() ||
                !conversion->second.method ||
                conversion->second.symbol.empty()) {
                mContext.error("`?` cannot convert error '" + sourceError->toString() +
                      "' to '" + targetError->toString() +
                      "'; implement `From<" + sourceError->toString() +
                      "> for " + targetError->toString() + "`",
                      propagation->line, propagation->col);
                return TyUnknown;
            }
            propagation->errorConversionSymbol =
                conversion->second.symbol;
        }
        propagation->resultType = result;
        propagation->propagatedResultType = functionResult;
        propagation->valueType = result->typeArgs[0];
        propagation->errorType = sourceError;
        propagation->propagatedErrorType = targetError;
        return propagation->valueType;
}

TypePtr BodyAnalyzer::analyzeLambdaExpr(LambdaExpr* le) {
        // Analyze lambda: enter new scope, register params, analyze body
        TypePtr savedReturn = mContext.mCurrentReturnType;
        bool savedSawReturn = mContext.mSawReturn;
        bool savedReturnsLinear =
            mContext.mCurrentFunctionReturnsLinear;
        auto savedReturnUsage =
            mContext.mCurrentFunctionReturnUsage;
        const size_t lambdaScopeDepth =
            mContext.mSymTable.depth();
        mCaptureFrames.push_back({lambdaScopeDepth, {}});
        mContext.mSymTable.enterScope();
        for (auto& p : le->params) {
            TypePtr pt = p.inferredType ? p.inferredType : mContext.declaredType(p.type.get(), {});
            p.inferredType = pt;
            SymbolInfo info;
            info.kind = SymbolKind::Variable;
            info.type = pt;
            const bool explicitUsage = p.hasExplicitUsage || p.isLinear ||
                dynamic_cast<LinearTypeAST*>(p.type.get()) ||
                dynamic_cast<AffineTypeAST*>(p.type.get());
            info.usage = p.isLinear ? luna::ownership::Usage::Linear
                : (explicitUsage ? p.usage : defaultUsageForType(pt));
            const auto contract = parameterContractFor(pt, info.usage, explicitUsage);
            info.relation = contract.relation;
            info.usage = contract.usage;
            info.isLinear = info.usage == luna::ownership::Usage::Linear;
            p.usage = info.usage;
            p.relation = info.relation;
            mContext.mSymTable.define(p.name, info);
        }
        TypePtr bodyRet = le->returnType ? mContext.declaredType(le->returnType.get(), {}) : mContext.mConstraints.fresh();
        const bool returnsLinear =
            dynamic_cast<LinearTypeAST*>(
                le->returnType.get()) != nullptr;
        const bool returnsAffine =
            dynamic_cast<AffineTypeAST*>(
                le->returnType.get()) != nullptr;
        const auto returnUsage = returnsLinear
            ? luna::ownership::Usage::Linear
            : (returnsAffine
                   ? luna::ownership::Usage::Affine
                   : defaultUsageForType(bodyRet));
        mContext.mCurrentReturnType = bodyRet;
        mContext.mCurrentFunctionReturnsLinear =
            returnsLinear;
        mContext.mCurrentFunctionReturnUsage =
            returnUsage;
        mContext.mSawReturn = false;
        if (le->body) analyzeBlock(le->body.get(), bodyRet);
        if (!mContext.mSawReturn) mContext.constrain(bodyRet, TyUnit, "lambda without a return value");
        mContext.mSymTable.exitScope();
        mContext.mCurrentReturnType = savedReturn;
        mContext.mCurrentFunctionReturnsLinear =
            savedReturnsLinear;
        mContext.mCurrentFunctionReturnUsage =
            savedReturnUsage;
        mContext.mSawReturn = savedSawReturn;

        CaptureFrame frame = std::move(mCaptureFrames.back());
        mCaptureFrames.pop_back();
        if (!mCaptureFrames.empty()) {
            auto& enclosing = mCaptureFrames.back();
            for (const auto& name : frame.captures) {
                const auto* symbol = mContext.lookupSymbol(name);
                if (!symbol || symbol->kind != SymbolKind::Variable)
                    continue;
                if (mContext.mSymTable.lookupDepth(name) >=
                    enclosing.lambdaScopeDepth)
                    continue;
                if (std::find(enclosing.captures.begin(),
                              enclosing.captures.end(), name) ==
                    enclosing.captures.end())
                    enclosing.captures.push_back(name);
            }
        }

        // Build closure function type: fn(ParamTypes) -> ReturnType
        TypeVec paramTypes;
        std::vector<luna::ownership::Contract> paramContracts;
        for (auto& p : le->params) {
            paramTypes.push_back(p.inferredType);
            paramContracts.push_back({p.relation, p.usage});
        }
        TypePtr retType = bodyRet;

        // Capture resolution: recorded free variables become closure
        // environment fields. Copy captures copy by value; Affine/Linear
        // captures move the outer binding into the environment (C016 CL010).
        // Borrowed (Reference) captures remain a later slice. Function
        // references are not captures.
        std::vector<std::string> captures;
        std::vector<TypeField> captureFields;
        bool captureError = false;
        for (const auto& name : frame.captures) {
            const auto* symbol = mContext.lookupSymbol(name);
            if (!symbol || symbol->kind != SymbolKind::Variable)
                continue;
            if (symbol->type &&
                symbol->type->kind == TypeKind::Reference) {
                mContext.error(
                    "lambda capture of borrowed binding '" + name +
                    "' is not yet supported (C016 CL005)", le->line, le->col);
                captureError = true;
                continue;
            }
            captures.push_back(name);
            captureFields.push_back({name, symbol->type});
        }
        // CL009 requires one canonical capture order shared by Sema, the
        // builder, and the verifier. `Type::makeClosure` keeps fields in
        // name order, so the capture list must use the same order here.
        std::vector<size_t> captureOrder(captureFields.size());
        for (size_t index = 0; index < captureOrder.size(); ++index)
            captureOrder[index] = index;
        std::stable_sort(captureOrder.begin(), captureOrder.end(),
                         [&](size_t lhs, size_t rhs) {
                             return captureFields[lhs].name <
                                    captureFields[rhs].name;
                         });
        std::vector<std::string> orderedCaptures;
        std::vector<TypeField> orderedFields;
        orderedCaptures.reserve(captureOrder.size());
        orderedFields.reserve(captureOrder.size());
        for (const size_t index : captureOrder) {
            orderedCaptures.push_back(captures[index]);
            orderedFields.push_back(std::move(captureFields[index]));
        }
        captures = std::move(orderedCaptures);
        captureFields = std::move(orderedFields);
        if (!captureError) {
            le->captures = captures;
            le->envParamName = captures.empty()
                ? "" : "$closure.env";
            if (captures.empty()) {
                le->closureType = Type::makeFunction(
                    paramTypes, retType, std::move(paramContracts),
                    {luna::ownership::Relation::Owned, returnUsage});
            } else {
                le->closureType = Type::makeClosure(
                    paramTypes, retType, std::move(paramContracts),
                    {luna::ownership::Relation::Owned, returnUsage},
                    std::move(captureFields));
            }
        } else {
            le->closureType = Type::makeFunction(
                paramTypes, retType, std::move(paramContracts),
                {luna::ownership::Relation::Owned, returnUsage});
        }
        return le->closureType;
}
