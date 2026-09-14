#include "BodyAnalyzer.h"

#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"
#include "SemanticAnalysisSupport.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include "BodyAnalyzerInternal.h"

TypePtr BodyAnalyzer::analyzeMatchStmt(MatchStmt* match, TypePtr expectedReturn) {
    TypePtr matched = mContext.resolved(analyzeExpr(match->scrutinee.get()));
    match->matchedType = matched;

    bool hasCompileTimeOptional = false;
    bool compileTimeOptionalHasValue = false;
    std::string compileTimeDeclarationId;
    luna::identity::SymbolId compileTimeSymbolId;
    luna::identity::ContractId compileTimeContractId;
    if (auto* source = dynamic_cast<CallExpr*>(match->scrutinee.get());
        source && source->isCompileTimeOptionalDeclarationRef) {
        hasCompileTimeOptional = true;
        compileTimeOptionalHasValue = source->compileTimeOptionalHasValue;
        compileTimeDeclarationId = source->compileTimeDeclarationId;
        compileTimeSymbolId = source->resolvedSymbolId;
        compileTimeContractId = source->resolvedContractId;
    } else if (auto* identifier = dynamic_cast<IdentifierExpr*>(match->scrutinee.get())) {
        if (auto* symbol = mContext.lookupSymbol(identifier->name);
            symbol && symbol->isCompileTimeOptionalDeclarationRef) {
            hasCompileTimeOptional = true;
            compileTimeOptionalHasValue = symbol->compileTimeOptionalHasValue;
            compileTimeDeclarationId = symbol->compileTimeDeclarationId;
            compileTimeSymbolId = symbol->compileTimeDeclarationSymbolId;
            compileTimeContractId = symbol->compileTimeDeclarationContractId;
        }
    }
    match->isCompileTimeOptionalMatch = hasCompileTimeOptional;
    match->compileTimeSelectedArm = static_cast<size_t>(-1);

    struct VariantView {
        std::string name;
        size_t physicalIndex = 0;
        TypeVec fields;
    };
    std::vector<VariantView> variants;
    if (matched && matched->kind == TypeKind::Enum) {
        for (size_t index = 0; index < matched->variants.size(); ++index)
            variants.push_back(
                {matched->variants[index].name, index, matched->variants[index].fields});
    } else if (matched && matched->kind == TypeKind::Result && matched->typeArgs.size() == 2) {
        // Result's frozen ABI uses false/0 for Err and true/1 for Ok.
        variants.push_back({"Err", 0, {matched->typeArgs[1]}});
        variants.push_back({"Ok", 1, {matched->typeArgs[0]}});
    } else {
        mContext.error("match requires an enum or Result value", match->line, match->col);
        return TyUnit;
    }

    std::unordered_set<std::string> seenVariants;
    for (size_t armIndex = 0; armIndex < match->arms.size(); ++armIndex) {
        auto& arm = match->arms[armIndex];
        const auto selected =
            std::find_if(variants.begin(), variants.end(), [&](const VariantView& variant) {
                return variant.name == arm.variantName;
            });
        if (selected == variants.end()) {
            mContext.error("unknown variant '" + arm.variantName + "' in match on '" +
                               matched->toString() + "'",
                           arm.line, arm.col);
            continue;
        }
        std::string qualifierName = arm.typeQualifier;
        const size_t qualifierSeparator = qualifierName.rfind("::");
        if (qualifierSeparator != std::string::npos)
            qualifierName = qualifierName.substr(qualifierSeparator + 2);
        if (!arm.typeQualifier.empty() && qualifierName != matched->name &&
            arm.typeQualifier != matched->toString()) {
            mContext.error("match pattern qualifier '" + arm.typeQualifier +
                               "' does not name matched type '" + matched->toString() + "'",
                           arm.line, arm.col);
        }
        if (!seenVariants.insert(arm.variantName).second)
            mContext.error("duplicate match arm for variant '" + arm.variantName + "'", arm.line,
                           arm.col);
        if (arm.bindings.size() != selected->fields.size()) {
            mContext.error("variant '" + arm.variantName + "' expects " +
                               std::to_string(selected->fields.size()) +
                               " payload binding(s), got " + std::to_string(arm.bindings.size()),
                           arm.line, arm.col);
        }
        arm.variantIndex = selected->physicalIndex;
        const bool isSelectedCompileTimeArm =
            hasCompileTimeOptional &&
            arm.variantName == (compileTimeOptionalHasValue ? "Some" : "None");
        if (isSelectedCompileTimeArm) match->compileTimeSelectedArm = armIndex;
        arm.bindingTypes = selected->fields;
        arm.bindingUsages.clear();
        for (size_t index = 0; index < arm.bindingTypes.size(); ++index) {
            const auto blockDefault = index < arm.bindingUsageDefaults.size()
                                          ? arm.bindingUsageDefaults[index]
                                          : luna::ownership::Usage::Copy;
            arm.bindingUsages.push_back(luna::ownership::strongerUsage(
                blockDefault, defaultUsageForType(arm.bindingTypes[index])));
        }
        if (matched->kind == TypeKind::Enum && !matched->declarationLinkageName.empty()) {
            if (!arm.typeQualifier.empty())
                mContext.recordResolvedReference(arm.sourcePath, arm.qualifierLine,
                                                 arm.qualifierCol, arm.typeQualifier.size(),
                                                 matched->declarationLinkageName);
            mContext.recordResolvedReference(
                arm.sourcePath, arm.line, arm.col, arm.variantName.size(),
                matched->declarationLinkageName + "::variant::" + arm.variantName);
        }

        mContext.mSymTable.enterScope();
        std::unordered_set<std::string> seenBindings;
        const size_t count = std::min(arm.bindings.size(), arm.bindingTypes.size());
        for (size_t index = 0; index < count; ++index) {
            if (!seenBindings.insert(arm.bindings[index]).second) {
                mContext.error("duplicate payload binding '" + arm.bindings[index] + "'", arm.line,
                               arm.col);
                continue;
            }
            SymbolInfo binding;
            binding.kind = SymbolKind::Variable;
            binding.type = arm.bindingTypes[index];
            binding.usage = index < arm.bindingUsages.size()
                                ? arm.bindingUsages[index]
                                : defaultUsageForType(arm.bindingTypes[index]);
            binding.isLinear = binding.usage == luna::ownership::Usage::Linear;
            if (hasCompileTimeOptional && arm.variantName == "Some" && index == 0)
                binding.isCompileTimeOptionalPayload = true;
            if (isSelectedCompileTimeArm && compileTimeOptionalHasValue &&
                arm.variantName == "Some" && index == 0) {
                binding.compileTimeDeclarationId = compileTimeDeclarationId;
                binding.compileTimeDeclarationSymbolId = compileTimeSymbolId;
                binding.compileTimeDeclarationContractId = compileTimeContractId;
            }
            mContext.mSymTable.define(arm.bindings[index], binding);
        }
        const bool savedInactiveBranch = mInCompileTimeInactiveBranch;
        mInCompileTimeInactiveBranch =
            savedInactiveBranch || (hasCompileTimeOptional && !isSelectedCompileTimeArm);
        analyzeBlock(arm.body.get(), expectedReturn);
        mInCompileTimeInactiveBranch = savedInactiveBranch;
        mContext.mSymTable.exitScope();
    }

    if (seenVariants.size() != variants.size()) {
        std::string missing;
        for (const auto& variant : variants) {
            if (seenVariants.count(variant.name)) continue;
            if (!missing.empty()) missing += ", ";
            missing += variant.name;
        }
        if (matched->kind == TypeKind::Result) {
            mContext.error("Result match must contain exactly one `Ok` arm "
                           "and one `Err` arm",
                           match->line, match->col);
        } else {
            mContext.error("match on '" + matched->toString() +
                               "' is not exhaustive; missing variant(s): " + missing,
                           match->line, match->col);
        }
    }
    return TyUnit;
}

TypePtr BodyAnalyzer::analyzeRecordLiteralExpr(RecordLiteralExpr* record) {
    std::vector<std::pair<RecordLiteralExpr::Field*, TypePtr>> fields;
    fields.reserve(record->fields.size());
    std::set<std::string> names;
    for (auto& field : record->fields) {
        if (!names.insert(field.name).second) {
            mContext.error("duplicate record field '" + field.name + "'", field.line, field.col);
        }
        fields.push_back({&field, analyzeExpr(field.value.get())});
    }
    if (record->targetType) {
        record->recordType =
            mContext.resolved(mContext.resolveTypeAST(record->targetType.get(), {}));
        if (!record->recordType || record->recordType->kind != TypeKind::Struct) {
            mContext.error("named record construction requires a struct type", record->line,
                           record->col);
            return TyUnknown;
        }
        std::set<std::string> initialized;
        for (auto& entry : fields) {
            auto* field = entry.first;
            const auto& type = entry.second;
            auto declared = std::find_if(
                record->recordType->fields.begin(), record->recordType->fields.end(),
                [&](const TypeField& candidate) { return candidate.name == field->name; });
            if (declared == record->recordType->fields.end()) {
                mContext.error("struct '" + record->recordType->toString() + "' has no field '" +
                                   field->name + "'",
                               field->line, field->col);
                continue;
            }
            initialized.insert(field->name);
            mContext.constrain(type, declared->type, "field '" + field->name + "' initializer");
        }
        for (const auto& declared : record->recordType->fields) {
            if (initialized.find(declared.name) == initialized.end())
                mContext.error("named construction of '" + record->recordType->toString() +
                                   "' is missing field '" + declared.name + "'",
                               record->line, record->col);
        }
        return record->recordType;
    }
    std::vector<TypeField> structuralFields;
    structuralFields.reserve(fields.size());
    for (auto& entry : fields)
        structuralFields.push_back({entry.first->name, std::move(entry.second)});
    record->recordType = Type::makeRecord(std::move(structuralFields));
    return record->recordType;
}

TypePtr BodyAnalyzer::analyzeVariantConstructExpr(VariantConstructExpr* variant) {
    auto nominalIt = mContext.mDeclaredTypes.find(mContext.sourceDeclarationKey(variant->typeName));
    if (nominalIt == mContext.mDeclaredTypes.end() || nominalIt->second->kind != TypeKind::Enum) {
        mContext.error("'" + variant->typeName + "' is not an enum type");
        return TyUnknown;
    }

    TypeVec typeArgs;
    for (auto& arg : variant->typeArgs)
        typeArgs.push_back(mContext.resolveTypeAST(arg.get(), {}));
    if (typeArgs.empty()) {
        for (size_t i = 0; i < nominalIt->second->typeParams.size(); ++i)
            typeArgs.push_back(mContext.mConstraints.fresh());
    } else if (typeArgs.size() != nominalIt->second->typeParams.size()) {
        mContext.error("Enum '" + variant->typeName + "' expects " +
                       std::to_string(nominalIt->second->typeParams.size()) + " type arguments");
    }

    auto constructed = mContext.instantiateNominal(nominalIt->second, typeArgs);
    variant->constructedType = constructed;
    mContext.mInferenceRoots.emplace_back(constructed, "type arguments of '" + variant->typeName +
                                                           "::" + variant->variantName + "'");
    const TypeVariant* selected = nullptr;
    for (auto& candidate : constructed->variants) {
        if (candidate.name == variant->variantName) {
            selected = &candidate;
            break;
        }
    }
    if (!selected) {
        mContext.error("Enum '" + variant->typeName + "' has no variant '" + variant->variantName +
                       "'");
        return TyUnknown;
    }
    const std::string enumLinkage = nominalIt->second->declarationLinkageName;
    if (!enumLinkage.empty()) {
        mContext.recordResolvedReference(variant->typeSourcePath, variant->typeLine,
                                         variant->typeCol, variant->typeName.size(), enumLinkage);
        mContext.recordResolvedReference(variant->sourcePath, variant->line, variant->col,
                                         variant->variantName.size(),
                                         enumLinkage + "::variant::" + variant->variantName);
    }
    if (selected->fields.size() != variant->args.size()) {
        mContext.error("Variant '" + variant->variantName + "' expects " +
                       std::to_string(selected->fields.size()) + " arguments");
        return constructed;
    }
    for (size_t i = 0; i < variant->args.size(); ++i) {
        TypePtr actual = analyzeExpr(variant->args[i].get());
        const TypePtr expected = mContext.resolved(selected->fields[i]);
        // Literals are representationally polymorphic at a statically
        // known enum field, matching call and FFI argument behavior.
        if (dynamic_cast<IntLiteralExpr*>(variant->args[i].get()) && isNumericType(expected))
            continue;
        if (dynamic_cast<StringLiteralExpr*>(variant->args[i].get()) &&
            expected->kind == TypeKind::CStr)
            continue;
        mContext.constrain(actual, expected, "enum variant argument");
    }
    return constructed;
}
