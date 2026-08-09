#pragma once

#include "SemanticContextAccess.h"

class TypeResolver final : public TypeAnalysis {
public:
    explicit TypeResolver(TypeContextAccess context)
        : mContext(std::move(context)) {}

    FunctionDecl* findMatchingImpl(
        const std::string& traitName, const std::string& typeName,
        const std::string& methodName) override;
    FunctionDecl* monomorphize(
        FunctionDecl* generic, const TypeVec& concreteTypes) override;
    TypePtr resolveTypeAST(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings) override;
    TypePtr instantiateNominal(
        const TypePtr& type, const std::vector<TypePtr>& args) override;
    TypePtr declaredType(
        const TypeAST* ast,
        const std::unordered_map<std::string, TypePtr>& bindings) override;
    TypePtr resolved(const TypePtr& type) override;
    bool constrain(
        const TypePtr& actual, const TypePtr& expected,
        const std::string& context) override;
    void requireBool(
        const TypePtr& type, const std::string& context) override;
    void requireNumeric(
        const TypePtr& type, const std::string& context) override;
    void requireInteger(
        const TypePtr& type, const std::string& context) override;
    void checkUnresolved(
        const TypePtr& type, const std::string& context) override;
    std::unique_ptr<TypeAST> typeToAST(const TypePtr& type) override;
    void materializeInferredTypes(Program* program) override;

private:
    TypeContextAccess mContext;
};
