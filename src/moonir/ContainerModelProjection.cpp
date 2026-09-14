#include "ContainerModelInternal.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moon {
namespace container_detail {

bool isGenericRecipe(const FunctionDecl& function) {
    return !function.typeParams.empty() && !function.isTemplateInstance;
}

void collectConcreteFunctions(
    const std::vector<std::unique_ptr<Decl>>& declarations,
    std::vector<const FunctionDecl*>& functions) {
    for (const auto& declaration : declarations) {
        if (!declaration) continue;
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (!isGenericRecipe(*function)) functions.push_back(function);
            continue;
        }
        if (const auto* implementation =
                dynamic_cast<const ImplDecl*>(declaration.get())) {
            for (const auto& method : implementation->methods)
                if (method && !isGenericRecipe(*method))
                    functions.push_back(method.get());
        }
    }
}

const DeclarationRecord* findDeclarationRecord(
    const Module& module, const DeclarationRef& reference) {
    for (const auto& declaration : module.declarationTable)
        if (declaration.symbolId == reference.symbol &&
            declaration.contractId == reference.contract)
            return &declaration;
    return nullptr;
}

bool containsGenericRecipe(const Module& module) {
    for (const auto& type : module.typeTable)
        if (type.kind == TypeKind::TypeParam ||
            type.kind == TypeKind::InferenceVar ||
            type.kind == TypeKind::Unknown)
            return true;
    for (const auto& declaration : module.declarations) {
        if (!declaration) continue;
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (isGenericRecipe(*function)) return true;
        } else if (const auto* structure =
                       dynamic_cast<const StructDecl*>(declaration.get())) {
            if (!structure->typeParams.empty()) return true;
        } else if (const auto* enumeration =
                       dynamic_cast<const EnumDecl*>(declaration.get())) {
            if (!enumeration->typeParams.empty()) return true;
        } else if (const auto* trait =
                       dynamic_cast<const TraitDecl*>(declaration.get())) {
            if (!trait->typeParams.empty()) return true;
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration.get())) {
            if (!implementation->typeParams.empty()) return true;
            for (const auto& method : implementation->methods)
                if (method && isGenericRecipe(*method)) return true;
        }
    }
    return false;
}

struct ProjectionReferences {
    std::unordered_set<std::string> types;
    std::unordered_set<std::string> declarations;
    std::unordered_set<std::string> schemas;

    void type(const TypeRef& reference) {
        if (!reference.empty()) types.insert(reference.value);
    }
    void declaration(const DeclarationRef& reference) {
        if (!reference.empty()) declarations.insert(reference.symbol.value);
    }
    void schema(const std::string& id) {
        if (!id.empty()) schemas.insert(id);
    }
};

void collectGraphReferences(
    const ControlFlowGraph& graph, ProjectionReferences& references);

void collectExpressionReferences(
    const Expr* expression, ProjectionReferences& references) {
    if (!expression) return;
    references.type(expression->type);
    if (const auto* identifier =
            dynamic_cast<const IdentifierExpr*>(expression)) {
        references.declaration(identifier->declaration);
    } else if (const auto* binary =
                   dynamic_cast<const BinaryExpr*>(expression)) {
        collectExpressionReferences(binary->lhs.get(), references);
        collectExpressionReferences(binary->rhs.get(), references);
    } else if (const auto* unary =
                   dynamic_cast<const UnaryExpr*>(expression)) {
        collectExpressionReferences(unary->operand.get(), references);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        collectExpressionReferences(call->callee.get(), references);
        for (const auto& argument : call->args)
            collectExpressionReferences(argument.get(), references);
        for (const auto& argument : call->typeArgs) references.type(argument);
        references.declaration(call->calleeRef);
        references.type(call->intrinsicType);
        references.type(call->iteratorInputType);
        references.type(call->iteratorOutputType);
        references.type(call->iteratorRecipeSourceType);
        references.type(call->iteratorCollectTargetType);
        references.type(call->iteratorCollectBuilderType);
        references.declaration(call->iteratorCollectBegin);
        references.declaration(call->iteratorCollectPush);
        references.declaration(call->iteratorCollectFinish);
    } else if (const auto* launch =
                   dynamic_cast<const LaunchExpr*>(expression)) {
        references.declaration(launch->kernelRef);
        collectExpressionReferences(launch->threads.get(), references);
        for (const auto& argument : launch->args)
            collectExpressionReferences(argument.get(), references);
    } else if (const auto* variant =
                   dynamic_cast<const VariantConstructExpr*>(expression)) {
        references.type(variant->constructedType);
        for (const auto& argument : variant->args)
            collectExpressionReferences(argument.get(), references);
    } else if (const auto* result =
                   dynamic_cast<const ResultConstructExpr*>(expression)) {
        collectExpressionReferences(result->payload.get(), references);
    } else if (const auto* field =
                   dynamic_cast<const FieldAccessExpr*>(expression)) {
        collectExpressionReferences(field->object.get(), references);
    } else if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
        collectExpressionReferences(index->object.get(), references);
        collectExpressionReferences(index->index.get(), references);
    } else if (const auto* length =
                   dynamic_cast<const SliceLengthExpr*>(expression)) {
        collectExpressionReferences(length->slice.get(), references);
    } else if (const auto* array =
                   dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        references.type(array->elementType);
        for (const auto& element : array->elements)
            collectExpressionReferences(element.get(), references);
    } else if (const auto* record =
                   dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            collectExpressionReferences(field.value.get(), references);
    } else if (const auto* allocation =
                   dynamic_cast<const HeapAllocExpr*>(expression)) {
        references.type(allocation->allocatedType);
        collectExpressionReferences(allocation->initializer.get(), references);
    } else if (const auto* allocation =
                   dynamic_cast<const InitAllocationExpr*>(expression)) {
        references.type(allocation->allocatedType);
        for (const auto& element : allocation->elements)
            collectExpressionReferences(element.value.get(), references);
    } else if (const auto* move = dynamic_cast<const MoveExpr*>(expression)) {
        collectExpressionReferences(move->operand.get(), references);
    } else if (const auto* borrow =
                   dynamic_cast<const BorrowExpr*>(expression)) {
        collectExpressionReferences(borrow->operand.get(), references);
    } else if (const auto* dereference =
                   dynamic_cast<const DerefExpr*>(expression)) {
        collectExpressionReferences(dereference->operand.get(), references);
    } else if (const auto* address =
                   dynamic_cast<const AddrOfExpr*>(expression)) {
        collectExpressionReferences(address->operand.get(), references);
    } else if (const auto* lambda =
                   dynamic_cast<const LambdaExpr*>(expression)) {
        for (const auto& parameter : lambda->params)
            references.type(parameter.type);
        references.type(lambda->returnType);
        references.type(lambda->closureType);
        if (lambda->controlFlow)
            collectGraphReferences(*lambda->controlFlow, references);
    } else if (const auto* closure =
                   dynamic_cast<const MakeClosureExpr*>(expression)) {
        collectExpressionReferences(closure->lambda.get(), references);
        for (const auto& value : closure->capturedValues)
            collectExpressionReferences(value.get(), references);
    } else if (const auto* assignment =
                   dynamic_cast<const AssignExpr*>(expression)) {
        collectExpressionReferences(assignment->lhs.get(), references);
        collectExpressionReferences(assignment->rhs.get(), references);
    }
}

void collectGraphReferences(
    const ControlFlowGraph& graph, ProjectionReferences& references) {
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (const auto* let = dynamic_cast<const LetStmt*>(operation.get())) {
                references.type(let->type);
                references.type(let->materializedIteratorSourceType);
                collectExpressionReferences(let->initializer.get(), references);
            } else if (const auto* allocation =
                           dynamic_cast<const AllocateStmt*>(operation.get())) {
                references.type(allocation->allocatedType);
            } else if (const auto* statement =
                           dynamic_cast<const ExprStmt*>(operation.get())) {
                collectExpressionReferences(statement->expr.get(), references);
            } else if (const auto* release =
                           dynamic_cast<const FreeStmt*>(operation.get())) {
                collectExpressionReferences(release->operand.get(), references);
            } else if (const auto* await =
                           dynamic_cast<const AwaitStmt*>(operation.get())) {
                collectExpressionReferences(await->event.get(), references);
            }
        }
        references.type(block.terminator.switchType);
        collectExpressionReferences(
            block.terminator.operand.get(), references);
    }
    for (const auto& region : graph.regions)
        references.declaration(region.fragment);
    for (const auto& local : graph.locals) references.type(local.type);
    for (const auto& cleanup : graph.cleanups) references.type(cleanup.type);
}

void collectFunctionReferences(
    const FunctionDecl& function, ProjectionReferences& references) {
    for (const auto& parameter : function.params)
        references.type(parameter.type);
    references.type(function.returnType);
    for (const auto& argument : function.concreteTypeArgs)
        references.type(argument);
    if (function.controlFlow)
        collectGraphReferences(*function.controlFlow, references);
}

bool buildConcreteProjection(
    const ContainerManifest& manifest, const Module& source,
    Module& projection, std::string& error) {
    std::unordered_set<std::string> genericRecipeSymbols;
    for (const auto& declaration : source.declarations) {
        if (!declaration) continue;
        const auto remember = [&](const Decl& recipe) {
            genericRecipeSymbols.insert(recipe.symbolId.value);
        };
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            if (isGenericRecipe(*function)) remember(*function);
        } else if (const auto* structure =
                       dynamic_cast<const StructDecl*>(declaration.get())) {
            if (!structure->typeParams.empty()) remember(*structure);
        } else if (const auto* enumeration =
                       dynamic_cast<const EnumDecl*>(declaration.get())) {
            if (!enumeration->typeParams.empty()) remember(*enumeration);
        } else if (const auto* trait =
                       dynamic_cast<const TraitDecl*>(declaration.get())) {
            if (!trait->typeParams.empty()) remember(*trait);
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration.get())) {
            if (!implementation->typeParams.empty()) remember(*implementation);
            for (const auto& method : implementation->methods)
                if (method && isGenericRecipe(*method)) remember(*method);
        }
    }

    std::unordered_map<std::string, size_t> typeIndexes;
    typeIndexes.reserve(source.typeTable.size());
    for (size_t index = 0; index < source.typeTable.size(); ++index)
        typeIndexes.emplace(source.typeTable[index].id.value, index);

    std::vector<bool> concrete(source.typeTable.size(), true);
    for (size_t index = 0; index < source.typeTable.size(); ++index) {
        const auto kind = source.typeTable[index].kind;
        if (kind == TypeKind::TypeParam || kind == TypeKind::InferenceVar ||
            kind == TypeKind::Unknown)
            concrete[index] = false;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = 0; index < source.typeTable.size(); ++index) {
            if (!concrete[index]) continue;
            for (const auto& reference :
                 source.typeTable[index].referencedTypeIds) {
                const auto found = typeIndexes.find(reference.value);
                if (found == typeIndexes.end() || !concrete[found->second]) {
                    concrete[index] = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    std::unordered_set<std::string> concreteTypes;
    for (size_t index = 0; index < source.typeTable.size(); ++index) {
        if (!concrete[index]) continue;
        concreteTypes.insert(source.typeTable[index].id.value);
    }
    std::unordered_map<std::string, const DeclarationRecord*>
        concreteDeclarations;
    for (const auto& declaration : source.declarationTable) {
        if (genericRecipeSymbols.count(declaration.symbolId.value) != 0 ||
            declaration.type.empty() ||
            concreteTypes.count(declaration.type.value) == 0)
            continue;
        concreteDeclarations.emplace(declaration.symbolId.value, &declaration);
    }

    std::vector<const FunctionDecl*> concreteFunctions;
    collectConcreteFunctions(source.declarations, concreteFunctions);
    std::unordered_map<std::string, const FunctionDecl*> functionsBySymbol;
    for (const auto* function : concreteFunctions)
        functionsBySymbol.emplace(function->symbolId.value, function);

    std::unordered_map<std::string, const MetadataSchema*> schemasById;
    for (const auto& schema : source.metadataSchemas)
        schemasById.emplace(schema.id, &schema);

    std::unordered_set<std::string> selectedTypes;
    std::unordered_set<std::string> selectedDeclarations;
    std::unordered_set<std::string> selectedSchemas;
    std::vector<std::string> pendingTypes;
    std::vector<std::string> pendingDeclarations;
    std::vector<std::string> pendingSchemas;
    const auto selectType = [&](const TypeRef& reference) {
        if (!reference.empty() && selectedTypes.insert(reference.value).second)
            pendingTypes.push_back(reference.value);
    };
    const auto selectDeclaration = [&](const DeclarationRef& reference) {
        if (!reference.empty() &&
            selectedDeclarations.insert(reference.symbol.value).second)
            pendingDeclarations.push_back(reference.symbol.value);
    };
    const auto selectSchema = [&](const std::string& schema) {
        if (!schema.empty() && selectedSchemas.insert(schema).second)
            pendingSchemas.push_back(schema);
    };

    if (!manifest.entrypoint.empty()) selectDeclaration(manifest.entrypoint);
    for (const auto& exported : source.exports)
        selectDeclaration(exported.declaration);
    for (const auto& import : source.imports)
        if (import.kind == ImportKind::Host)
            selectDeclaration(import.declaration);
    for (const auto& declaration : source.declarationTable) {
        if (declaration.retention != Retention::CompileTime &&
            concreteDeclarations.count(declaration.symbolId.value) != 0)
            selectDeclaration({declaration.symbolId, declaration.contractId});
    }

    size_t typeCursor = 0;
    size_t declarationCursor = 0;
    size_t schemaCursor = 0;
    while (typeCursor < pendingTypes.size() ||
           declarationCursor < pendingDeclarations.size() ||
           schemaCursor < pendingSchemas.size()) {
        while (declarationCursor < pendingDeclarations.size()) {
            const auto symbol = pendingDeclarations[declarationCursor++];
            const auto found = concreteDeclarations.find(symbol);
            if (found == concreteDeclarations.end()) {
                error = "Moon Container reachable declaration is generic or unresolved";
                return false;
            }
            const auto& declaration = *found->second;
            selectType(declaration.type);
            selectDeclaration(declaration.dropGlue);
            if (declaration.kind == DeclarationKind::MetadataSchema)
                selectSchema(declaration.id);
            for (const auto& metadata : declaration.metadata)
                selectSchema(metadata.schemaId);
            const auto function = functionsBySymbol.find(symbol);
            if (function != functionsBySymbol.end()) {
                ProjectionReferences references;
                collectFunctionReferences(*function->second, references);
                for (const auto& type : references.types)
                    selectType(TypeRef{type});
                for (const auto& dependency : references.declarations) {
                    const auto record = concreteDeclarations.find(dependency);
                    if (record == concreteDeclarations.end()) {
                        error = "Moon Container reachable code depends on a generic or missing declaration";
                        return false;
                    }
                    selectDeclaration({record->second->symbolId,
                                       record->second->contractId});
                }
                for (const auto& schema : references.schemas)
                    selectSchema(schema);
            }
        }
        while (typeCursor < pendingTypes.size()) {
            const auto id = pendingTypes[typeCursor++];
            if (concreteTypes.count(id) == 0) {
                error = "Moon Container reachable model depends on a generic or missing type";
                return false;
            }
            const auto found = typeIndexes.find(id);
            if (found == typeIndexes.end()) {
                error = "Moon Container reachable TypeId is absent from the frozen table";
                return false;
            }
            const auto& type = source.typeTable[found->second];
            for (const auto& dependency : type.referencedTypeIds)
                selectType(TypeRef{dependency.value});
            selectDeclaration(type.dropGlue);
        }
        while (schemaCursor < pendingSchemas.size()) {
            const auto id = pendingSchemas[schemaCursor++];
            const auto found = schemasById.find(id);
            if (found == schemasById.end()) {
                error = "Moon Container reachable declaration uses a missing metadata schema";
                return false;
            }
            for (const auto& field : found->second->fields)
                selectType(field.type);
        }
    }

    for (const auto& type : source.typeTable)
        if (selectedTypes.count(type.id.value) != 0)
            projection.typeTable.push_back(type);
    projection.typeTableSealed = source.typeTableSealed;
    for (const auto& declaration : source.declarationTable)
        if (selectedDeclarations.count(declaration.symbolId.value) != 0)
            projection.declarationTable.push_back(declaration);
    for (const auto& schema : source.metadataSchemas)
        if (selectedSchemas.count(schema.id) != 0)
            projection.metadataSchemas.push_back(schema);

    for (const auto& import : source.imports) {
        if (import.kind == ImportKind::Package ||
            selectedDeclarations.count(import.declaration.symbol.value) != 0)
            projection.imports.push_back(import);
    }
    projection.exports = source.exports;
    for (const auto& exported : projection.exports) {
        if (selectedDeclarations.count(exported.declaration.symbol.value) == 0 ||
            selectedTypes.count(exported.type.value) == 0) {
            error = "Moon Container export has a generic contract";
            return false;
        }
    }

    projection.formatMajor = source.formatMajor;
    projection.formatMinor = source.formatMinor;
    projection.name = source.name;
    projection.packageUses = source.packageUses;
    projection.isPackage = source.isPackage;
    projection.features = source.features;
    projection.rebuildIndexes();
    return true;
}


} // namespace container_detail
} // namespace moon
