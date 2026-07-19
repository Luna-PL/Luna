#include "TraitChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <sstream>

TraitChecker::TraitChecker() {}

static std::string traitIdOf(const TraitDecl* trait) {
    if (!trait) return "";
    return trait->generatedSymbolName.empty() ? trait->name : trait->generatedSymbolName;
}

static std::string typeIdOf(const TypePtr& type) {
    if (!type) return "?";
    if (!type->nominalId.empty()) return type->nominalId;
    return type->toString();
}

bool TraitChecker::check(Program* program) {
    registerTraits(program);
    registerImpls(program);

    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            mDiagnosticFile = f->sourcePath;
            mDiagnosticLine = f->line;
            mDiagnosticCol = f->col;
            checkConcreteFunction(f);
        }
    }

    return mErrors.empty();
}

void TraitChecker::registerTraits(Program* program) {
    for (auto& decl : program->declarations) {
        if (auto* td = dynamic_cast<TraitDecl*>(decl.get())) {
            std::vector<MethodSig> sigs;
            for (auto& m : td->methods) {
                MethodSig sig;
                sig.name = m.name;
                for (auto& p : m.params) {
                    TypePtr pt = resolveType(p.type.get(), {});
                    sig.paramTypes.push_back(pt);
                }
                sig.returnType = m.returnType ? resolveType(m.returnType.get(), {}) : TyUnit;
                sigs.push_back(std::move(sig));
            }
            mTraitSigs[traitIdOf(td)] = std::move(sigs);
        }
    }
}

void TraitChecker::registerImpls(Program* program) {
    for (auto& decl : program->declarations) {
        if (auto* id = dynamic_cast<ImplDecl*>(decl.get())) {
            const std::string traitId = id->trait.resolvedTraitId;
            if (traitId.empty()) continue; // semantic analysis already diagnosed it
            const std::string targetId = id->resolvedTargetTypeId.empty()
                ? "?" : id->resolvedTargetTypeId;
            for (auto& method : id->methods) {
                mImplMap[traitId][targetId][method->name] = method.get();
            }
        }
    }
}

void TraitChecker::checkConcreteFunction(FunctionDecl* decl) {
    if (decl->whereClauses.empty()) return;

    for (auto& clause : decl->whereClauses) {
        const std::string& typeParamName = clause.typeParam;
        const std::string& traitId = clause.trait.resolvedTraitId;
        if (traitId.empty()) {
            error("Unresolved trait reference '" + clause.trait.name + "' in where clause");
            continue;
        }

        // Check if type param is valid
        bool found = false;
        for (auto& tp : decl->typeParams) {
            if (tp == typeParamName) { found = true; break; }
        }
        if (!found) {
            error("Unknown type parameter '" + typeParamName + "' in where clause");
            continue;
        }

        // Check if trait exists
        if (mTraitSigs.find(traitId) == mTraitSigs.end()) {
            error("Unknown trait '" + traitId + "' in where clause");
            continue;
        }
    }
}

bool TraitChecker::satisfies(const TypePtr& type, const std::string& traitName) const {
    if (!type) return false;
    std::string typeName = typeIdOf(type);

    auto traitIt = mImplMap.find(traitName);
    if (traitIt == mImplMap.end()) return false;

    auto typeIt = traitIt->second.find(typeName);
    if (typeIt == traitIt->second.end()) {
        // Check if impl exists for the type
        return false;
    }

    auto traitSigsIt = mTraitSigs.find(traitName);
    if (traitSigsIt == mTraitSigs.end()) return true; // no methods required

    for (auto& sig : traitSigsIt->second) {
        if (typeIt->second.find(sig.name) == typeIt->second.end()) return false;
    }

    return true;
}

void TraitChecker::error(const std::string& msg) {
    mErrors.push_back(diagnostic::format(
        "trait", msg, mDiagnosticFile, mDiagnosticLine, mDiagnosticCol,
        "declare the type parameter and trait, then use a valid `where T: Trait` constraint"));
}
