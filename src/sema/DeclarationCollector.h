#pragma once

#include "SemanticContextAccess.h"

class DeclarationCollector final : public DeclarationAnalysis {
public:
    explicit DeclarationCollector(DeclarationContextAccess context)
        : mContext(std::move(context)) {}

    void declareFunction(FunctionDecl* decl) override;
    void declareMeta(MetaDecl* decl) override;
    void declareConstraint(ConstraintDecl* decl) override;
    void analyzeConstraint(ConstraintDecl* decl) override;
    void analyzeMeta(MetaDecl* decl) override;
    void validateMetadata(Decl* decl) override;
    void declareFragment(FragmentDecl* decl) override;
    bool isFFIType(
        const TypePtr& type, const std::string& context) override;
    void validateFFIFunction(FunctionDecl* decl) override;
    void declareStruct(StructDecl* decl) override;
    void declareEnum(EnumDecl* decl) override;
    void declareTrait(TraitDecl* decl) override;
    void declareImpl(ImplDecl* decl) override;

private:
    DeclarationContextAccess mContext;
};
