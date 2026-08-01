#include "tooling/SymbolIndex.h"

#include "parser/AST.h"

#include <algorithm>
#include <utility>

namespace luna::tooling {
namespace {

std::string typeName(const TypePtr& type) {
    return type ? type->toString() : "?";
}

std::string usagePrefix(luna::ownership::Usage usage) {
    switch (usage) {
        case luna::ownership::Usage::Copy: return {};
        case luna::ownership::Usage::Affine: return "affine ";
        case luna::ownership::Usage::Linear: return "linear ";
    }
    return {};
}

std::string typeParameters(const std::vector<std::string>& parameters) {
    if (parameters.empty()) return {};
    std::string result = "<";
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (index) result += ", ";
        result += parameters[index];
    }
    return result + ">";
}

std::string parameterList(const std::vector<Param>& parameters) {
    std::string result = "(";
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (index) result += ", ";
        const auto& parameter = parameters[index];
        result += parameter.name + ": " + usagePrefix(parameter.usage) +
            typeName(parameter.inferredType);
    }
    return result + ")";
}

std::string functionSignature(const FunctionDecl& function) {
    return "fn " + function.name + typeParameters(function.typeParams) +
        parameterList(function.params) + " -> " +
        usagePrefix(function.returnUsage) + typeName(function.inferredReturnType);
}

std::string qualifiedName(const Decl& declaration, const std::string& name) {
    std::string result = declaration.packageId.empty()
        ? std::string("main") : declaration.packageId;
    if (!declaration.modulePath.empty()) result += "::" + declaration.modulePath;
    return result + "::" + name;
}

void appendIdentityComponent(std::string& id, const std::string& component) {
    id += ":" + std::to_string(component.size()) + ":" + component;
}

std::string symbolId(const IndexedSymbol& symbol) {
    std::string result = "luna.symbol.v1";
    appendIdentityComponent(result, symbol.packageId);
    appendIdentityComponent(result, symbol.modulePath);
    appendIdentityComponent(result, indexedSymbolKindName(symbol.kind));
    appendIdentityComponent(result, symbol.linkageName);
    return result;
}

SymbolSourceLocation selectionOf(const Decl& declaration,
                                 const std::string& name) {
    return {
        declaration.sourcePath,
        declaration.nameLine > 0 ? declaration.nameLine : declaration.line,
        declaration.nameCol > 0 ? declaration.nameCol : declaration.col,
        name.size(),
    };
}

IndexedSymbol commonSymbol(const Decl& declaration, std::string name,
                           IndexedSymbolKind kind, std::string signature,
                           bool external = false) {
    IndexedSymbol symbol;
    symbol.name = std::move(name);
    symbol.packageId = declaration.packageId.empty()
        ? std::string("main") : declaration.packageId;
    symbol.modulePath = declaration.modulePath;
    symbol.qualifiedName = qualifiedName(declaration, symbol.name);
    symbol.linkageName = declaration.generatedSymbolName.empty()
        ? symbol.qualifiedName : declaration.generatedSymbolName;
    symbol.signature = std::move(signature);
    symbol.kind = kind;
    symbol.selection = selectionOf(declaration, symbol.name);
    symbol.exported = declaration.isExported;
    symbol.external = external;
    symbol.id = symbolId(symbol);
    return symbol;
}

IndexedSymbol childSymbol(const Decl& parent, std::string parentName,
                          std::string name, IndexedSymbolKind kind,
                          std::string signature,
                          SymbolSourceLocation selection,
                          const char* linkageComponent) {
    auto symbol = commonSymbol(parent, std::move(parentName), kind,
                               std::move(signature));
    const std::string parentQualified = symbol.qualifiedName;
    const std::string parentLinkage = parent.generatedSymbolName.empty()
        ? parentQualified : parent.generatedSymbolName;
    symbol.name = std::move(name);
    symbol.qualifiedName = parentQualified + "::" + symbol.name;
    symbol.linkageName = parentLinkage + "::" + linkageComponent + "::" +
        symbol.name;
    symbol.selection = std::move(selection);
    symbol.id = symbolId(symbol);
    return symbol;
}

} // namespace

const char* indexedSymbolKindName(IndexedSymbolKind kind) {
    switch (kind) {
        case IndexedSymbolKind::Function: return "function";
        case IndexedSymbolKind::Kernel: return "kernel";
        case IndexedSymbolKind::Method: return "method";
        case IndexedSymbolKind::Fragment: return "fragment";
        case IndexedSymbolKind::Struct: return "struct";
        case IndexedSymbolKind::Enum: return "enum";
        case IndexedSymbolKind::Trait: return "trait";
        case IndexedSymbolKind::Metadata: return "metadata";
        case IndexedSymbolKind::Constraint: return "constraint";
        case IndexedSymbolKind::Field: return "field";
        case IndexedSymbolKind::EnumVariant: return "enum-variant";
    }
    return "unknown";
}

SymbolIndex SymbolIndex::build(const Program& program) {
    SymbolIndex index;
    for (const auto& declarationOwner : program.declarations) {
        const auto* declaration = declarationOwner.get();
        if (const auto* function = dynamic_cast<const FunctionDecl*>(declaration)) {
            if (function->isTemplateInstance) continue;
            index.add(commonSymbol(
                *function, function->name,
                function->isKernel ? IndexedSymbolKind::Kernel
                                   : IndexedSymbolKind::Function,
                functionSignature(*function), function->isExtern));
        } else if (const auto* fragment =
                       dynamic_cast<const FragmentDecl*>(declaration)) {
            std::string signature = fragment->kind == FragmentKind::Interceptor
                ? "interceptor " : "context ";
            if (fragment->cardinality == FragmentCardinality::Many)
                signature += "many ";
            signature += fragment->name + parameterList(fragment->params);
            index.add(commonSymbol(
                *fragment, fragment->name, IndexedSymbolKind::Fragment,
                std::move(signature)));
        } else if (const auto* structure =
                       dynamic_cast<const StructDecl*>(declaration)) {
            index.add(commonSymbol(
                *structure, structure->name, IndexedSymbolKind::Struct,
                "struct " + structure->name +
                    typeParameters(structure->typeParams)));
            for (const auto& field : structure->fields) {
                index.add(childSymbol(
                    *structure, structure->name, field.name,
                    IndexedSymbolKind::Field,
                    field.name + ": " + typeName(field.inferredType),
                    {field.sourcePath, field.nameLine, field.nameCol,
                     field.name.size()},
                    "field"));
            }
        } else if (const auto* enumeration =
                       dynamic_cast<const EnumDecl*>(declaration)) {
            index.add(commonSymbol(
                *enumeration, enumeration->name, IndexedSymbolKind::Enum,
                "enum " + enumeration->name +
                    typeParameters(enumeration->typeParams)));
            for (const auto& variant : enumeration->variants) {
                std::string signature = variant.name + "(";
                for (size_t index = 0; index < variant.inferredFields.size();
                     ++index) {
                    if (index) signature += ", ";
                    signature += typeName(variant.inferredFields[index]);
                }
                signature += ")";
                index.add(childSymbol(
                    *enumeration, enumeration->name, variant.name,
                    IndexedSymbolKind::EnumVariant, std::move(signature),
                    {variant.sourcePath, variant.nameLine, variant.nameCol,
                     variant.name.size()},
                    "variant"));
            }
        } else if (const auto* trait =
                       dynamic_cast<const TraitDecl*>(declaration)) {
            index.add(commonSymbol(
                *trait, trait->name, IndexedSymbolKind::Trait,
                "trait " + trait->name + typeParameters(trait->typeParams)));
        } else if (const auto* metadata =
                       dynamic_cast<const MetaDecl*>(declaration)) {
            index.add(commonSymbol(
                *metadata, metadata->name, IndexedSymbolKind::Metadata,
                "meta " + metadata->name));
        } else if (const auto* constraint =
                       dynamic_cast<const ConstraintDecl*>(declaration)) {
            index.add(commonSymbol(
                *constraint, constraint->name, IndexedSymbolKind::Constraint,
                "constraint " + constraint->name +
                    typeParameters(constraint->typeParams)));
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration)) {
            for (const auto& method : implementation->methods) {
                auto symbol = commonSymbol(
                    *method, method->name, IndexedSymbolKind::Method,
                    functionSignature(*method), method->isExtern);
                // Method ownership follows the enclosing impl even for
                // parser-recovered methods whose common fields were partial.
                symbol.packageId = implementation->packageId.empty()
                    ? std::string("main") : implementation->packageId;
                symbol.modulePath = implementation->modulePath;
                symbol.qualifiedName = qualifiedName(*implementation, method->name);
                symbol.id = symbolId(symbol);
                index.add(std::move(symbol));
            }
        }
    }
    index.finalize();
    return index;
}

void SymbolIndex::add(IndexedSymbol symbol) {
    mDeclarations.push_back(std::move(symbol));
}

void SymbolIndex::finalize() {
    std::sort(mDeclarations.begin(), mDeclarations.end(),
              [](const IndexedSymbol& left, const IndexedSymbol& right) {
                  if (left.selection.path != right.selection.path)
                      return left.selection.path < right.selection.path;
                  if (left.selection.line != right.selection.line)
                      return left.selection.line < right.selection.line;
                  if (left.selection.column != right.selection.column)
                      return left.selection.column < right.selection.column;
                  return left.id < right.id;
              });
    for (size_t position = 0; position < mDeclarations.size(); ++position) {
        auto& symbol = mDeclarations[position];
        if (mById.count(symbol.id)) {
            appendIdentityComponent(symbol.id, symbol.selection.path);
            appendIdentityComponent(symbol.id, std::to_string(symbol.selection.line));
            appendIdentityComponent(symbol.id, std::to_string(symbol.selection.column));
        }
        mById.emplace(symbol.id, position);
        mByName.emplace(symbol.name, position);
        mByDocument.emplace(symbol.selection.path, position);
    }
}

const IndexedSymbol* SymbolIndex::findById(const std::string& id) const {
    const auto found = mById.find(id);
    return found == mById.end() ? nullptr : &mDeclarations[found->second];
}

std::vector<const IndexedSymbol*> SymbolIndex::findByName(
    const std::string& name) const {
    std::vector<const IndexedSymbol*> result;
    const auto matches = mByName.equal_range(name);
    for (auto match = matches.first; match != matches.second; ++match)
        result.push_back(&mDeclarations[match->second]);
    std::sort(result.begin(), result.end(),
              [](const IndexedSymbol* left, const IndexedSymbol* right) {
                  return left->id < right->id;
              });
    return result;
}

std::vector<const IndexedSymbol*> SymbolIndex::inDocument(
    const std::string& path) const {
    std::vector<const IndexedSymbol*> result;
    const auto matches = mByDocument.equal_range(path);
    for (auto match = matches.first; match != matches.second; ++match)
        result.push_back(&mDeclarations[match->second]);
    std::sort(result.begin(), result.end(),
              [](const IndexedSymbol* left, const IndexedSymbol* right) {
                  if (left->selection.line != right->selection.line)
                      return left->selection.line < right->selection.line;
                  return left->selection.column < right->selection.column;
              });
    return result;
}

} // namespace luna::tooling
