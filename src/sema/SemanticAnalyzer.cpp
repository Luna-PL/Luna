#include "SemanticAnalyzer.h"
#include "../parser/AST.h"
#include "../diagnostics/Diagnostic.h"
#include <sstream>
#include <functional>
#include <cmath>
#include <set>

static std::string displayTraitRef(const TraitRef& trait) {
    if (!trait.versionSelector) return trait.name;
    std::string result = trait.name + "@" + trait.versionSelector->tag + "(";
    result += trait.versionSelector->version
        ? trait.versionSelector->version->toString() : "";
    return result + ")";
}

static std::string displayDeclarationIdentity(
    const std::string& name, const std::optional<VersionTag>& tag) {
    if (!tag) return name;
    return name + "@" + tag->name + "(" + tag->version.toString() + ")";
}

SemanticAnalyzer::SemanticAnalyzer() {
    // Register built-in types
    mSymTable.defineType("i32", TyI32);
    mSymTable.defineType("i64", TyI64);
    mSymTable.defineType("f32", TyF32);
    mSymTable.defineType("f64", TyF64);
    mSymTable.defineType("bool", TyBool);
    mSymTable.defineType("string", TyString);

    // Register built-in print function (generic, handled by codegen)
    SymbolInfo printInfo;
    printInfo.kind = SymbolKind::Function;
    printInfo.typeParams = {"T"};
    printInfo.returnType = TyUnit;
    printInfo.paramTypes = {Type::makeTypeParam("T")};
    mSymTable.defineAtRoot("print", printInfo);
}

bool SemanticAnalyzer::analyze(Program* program) {
    mProgram = program;
    mConstScopes.clear();
    enterConstScope();
    mConstexprFunctions.clear();
    mSlotScopes.clear();
    mApplyScopes.clear();
    mDynamicApplyScopes.clear();
    enterSlotScope();
    mFragments.clear();
    mVersionedFunctions.clear();
    mVersionedFragments.clear();
    mVersionedNominals.clear();
    mTraits.clear();
    mVersionedTraits.clear();
    mTraitTypeParams.clear();
    mTraitMethods.clear();
    mImpls.clear();
    const size_t sourceDeclarationCount = program->declarations.size();

    // A package has one shared namespace even when its declarations come from
    // different files. Diagnose collisions here instead of allowing LLVM to
    // silently create suffixed symbols during code generation.
    std::unordered_map<std::string, Decl*> declaredNames;
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto* declaration = program->declarations[i].get();
        setDiagnosticLocation(declaration);
        std::string name;
        if (auto* f = dynamic_cast<FunctionDecl*>(declaration)) name = f->name;
        else if (auto* f = dynamic_cast<FragmentDecl*>(declaration)) name = f->name;
        else if (auto* s = dynamic_cast<StructDecl*>(declaration)) name = s->name;
        else if (auto* e = dynamic_cast<EnumDecl*>(declaration)) name = e->name;
        else if (auto* t = dynamic_cast<TraitDecl*>(declaration)) name = t->name;
        if (!name.empty()) {
            declaration->generatedSymbolName = versionedDeclarationName(
                name, declaration->versionTag);
            std::string identity = declaration->generatedSymbolName;
            if (!declaredNames.emplace(identity, declaration).second)
                error("Duplicate package declaration '" +
                      displayDeclarationIdentity(name, declaration->versionTag) + "'");
        }
        if (declaration->isExported && dynamic_cast<ImplDecl*>(declaration))
            error("Only functions, types, and traits can be exported; 'impl' is an internal declaration");
    }
    // Bind every nominal name before resolving any field. This permits
    // forward references and makes the declaration identity independent from
    // the order in which source declarations appear.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* s = dynamic_cast<StructDecl*>(program->declarations[i].get())) {
            const std::string identity = s->generatedSymbolName.empty()
                ? s->name : s->generatedSymbolName;
            if (!mNominalTypes.count(identity)) {
                auto type = Type::makeStruct(s->name, {}, identity);
                type->typeParams = s->typeParams;
                mNominalTypes[identity] = type;
                mSymTable.defineType(identity, type);
                if (s->versionTag) {
                    mVersionedNominals[s->name][s->versionTag->name].push_back(
                        {s->versionTag->version, type});
                } else {
                    mSymTable.defineType(s->name, type);
                }
            }
        } else if (auto* e = dynamic_cast<EnumDecl*>(program->declarations[i].get())) {
            const std::string identity = e->generatedSymbolName.empty()
                ? e->name : e->generatedSymbolName;
            if (!mNominalTypes.count(identity)) {
                auto type = Type::makeEnum(e->name, {}, identity);
                type->typeParams = e->typeParams;
                mNominalTypes[identity] = type;
                mSymTable.defineType(identity, type);
                if (e->versionTag) {
                    mVersionedNominals[e->name][e->versionTag->name].push_back(
                        {e->versionTag->version, type});
                } else {
                    mSymTable.defineType(e->name, type);
                }
            }
        }
    }
    // Pass 1a: register every trait identity before impls or generic
    // constraints are touched. This makes version selection declaration-order
    // independent, just as nominal type binding above is.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        if (auto* t = dynamic_cast<TraitDecl*>(decl.get())) {
            setDiagnosticLocation(t);
            declareTrait(t);
        }
    }
    // Pass 1b: declare all remaining types, functions, fragments, and impls.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        setDiagnosticLocation(decl.get());
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) declareFunction(f);
        else if (auto* f = dynamic_cast<FragmentDecl*>(decl.get())) declareFragment(f);
        else if (auto* s = dynamic_cast<StructDecl*>(decl.get())) declareStruct(s);
        else if (auto* e = dynamic_cast<EnumDecl*>(decl.get())) declareEnum(e);
        else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) declareImpl(i);
    }
    // Pass 2a: materialize all trait method sets before checking any impl or
    // generic call. This also supports trait declarations after their uses.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* t = dynamic_cast<TraitDecl*>(program->declarations[i].get())) {
            setDiagnosticLocation(t);
            analyzeTrait(t);
        }
    }
    // Pass 2b: analyze all ordinary bodies.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        setDiagnosticLocation(decl.get());
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) analyzeFunction(f);
        else if (auto* s = dynamic_cast<StructDecl*>(decl.get())) analyzeStruct(s);
        else if (auto* e = dynamic_cast<EnumDecl*>(decl.get())) analyzeEnum(e);
        else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) analyzeImpl(i);
    }
    // Numeric constraints have a useful, deterministic default. Other
    // unresolved variables are diagnosed because silently turning them into
    // a machine type would make inference depend on codegen details.
    mConstraints.defaultUnconstrainedNumeric();
    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (f->isExtern) continue;
            for (auto& p : f->params)
                checkUnresolved(p.inferredType, "parameter '" + p.name + "' of '" + f->name + "'");
            checkUnresolved(f->inferredReturnType, "return type of '" + f->name + "'");
        } else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& f : i->methods) {
                for (auto& p : f->params)
                    checkUnresolved(p.inferredType, "parameter '" + p.name + "' of '" + f->name + "'");
                checkUnresolved(f->inferredReturnType, "return type of '" + f->name + "'");
            }
        }
    }
    for (auto& root : mInferenceRoots)
        checkUnresolved(root.first, root.second);
    materializeInferredTypes(program);
    return mErrors.empty();
}

// ─── Declaration pass ──────────────────────────────────────────────

void SemanticAnalyzer::declareFunction(FunctionDecl* decl) {
    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) {
        bindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& clause : decl->whereClauses)
        resolveTraitRef(clause.trait, decl);
    SymbolInfo info;
    info.kind = SymbolKind::Function;
    info.isExported = decl->isExported;
    info.isExtern = decl->isExtern;
    info.returnsLinear = decl->returnsLinear;
    info.returnType = declaredType(decl->returnType.get(), bindings);
    if ((decl->isExtern || !decl->abi.empty()) && !decl->returnType)
        info.returnType = TyUnit;
    decl->inferredReturnType = info.returnType;
    info.typeParams = decl->typeParams;
    info.genericDecl = decl;
    for (auto& p : decl->params) {
        TypePtr pt = declaredType(p.type.get(), bindings);
        p.inferredType = pt;
        info.paramTypes.push_back(pt);
    }
    if (decl->versionTag) {
        mVersionedFunctions[decl->name][decl->versionTag->name].push_back(decl);
    } else {
        mSymTable.defineAtRoot(decl->name, info);
    }
    if (decl->isConstexpr)
        mConstexprFunctions[decl->generatedSymbolName.empty()
            ? decl->name : decl->generatedSymbolName] = decl;
    if (decl->isExtern || (decl->isExported && !decl->abi.empty()))
        validateFFIFunction(decl);
}

void SemanticAnalyzer::declareFragment(FragmentDecl* decl) {
    if (decl->versionTag) {
        auto& versions = mVersionedFragments[decl->name][decl->versionTag->name];
        if (!versions.empty() &&
            (versions.front()->kind != decl->kind ||
             versions.front()->cardinality != decl->cardinality)) {
            error("versioned fragment '" + decl->name + "@" + decl->versionTag->name +
                  "' changes its interceptor/context or once/many contract",
                  decl->line, decl->col);
            return;
        }
        versions.push_back(decl);
    } else if (!mFragments.emplace(decl->name, decl).second) {
        error("duplicate fragment declaration '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    SymbolInfo info;
    info.kind = SymbolKind::Fragment;
    info.type = Type::makeFragment(
        {}, TyUnit, decl->cardinality == FragmentCardinality::Many,
        decl->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context);
    if (!decl->versionTag) mSymTable.defineAtRoot(decl->name, info);
}

bool SemanticAnalyzer::isFFIType(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
        case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
        case TypeKind::USize: case TypeKind::ISize:
        case TypeKind::F32: case TypeKind::F64:
        case TypeKind::CStr: case TypeKind::RawPointer: case TypeKind::Unit:
            return true;
        case TypeKind::Reference:
            if (t->inner && t->inner->kind != TypeKind::Struct &&
                t->inner->kind != TypeKind::Record && t->inner->kind != TypeKind::Enum &&
                t->inner->kind != TypeKind::RawPointer)
                return isFFIType(t->inner, context);
            break;
        default:
            break;
    }
    error("Unsupported FFI type " + t->toString() + " in " + context);
    return false;
}

void SemanticAnalyzer::validateFFIFunction(FunctionDecl* decl) {
    if (!decl->abi.empty() && decl->abi != "C")
        error("Unsupported ABI '" + decl->abi + "' for function '" + decl->name + "'");
    if (decl->isExtern && decl->isExported)
        error("An extern function cannot also be exported: '" + decl->name + "'");
    if (decl->isExtern && decl->isConstexpr)
        error("An extern function cannot be constexpr: '" + decl->name + "'");
    if (!decl->typeParams.empty())
        error("C ABI function cannot be generic: '" + decl->name + "'");
    if (decl->returnsLinear &&
        (!decl->inferredReturnType || resolved(decl->inferredReturnType)->kind != TypeKind::RawPointer)) {
        error("owning FFI return of '" + decl->name + "' must use `linear raw<T>`");
    }
    for (auto& param : decl->params) {
        if (!param.type) {
            error("C ABI parameter '" + param.name + "' of '" + decl->name +
                  "' requires an explicit type");
            continue;
        }
        isFFIType(param.inferredType, "parameter '" + param.name + "' of '" + decl->name + "'");
    }
    isFFIType(decl->inferredReturnType,
              "return type of FFI function '" + decl->name + "'");
}

void SemanticAnalyzer::declareStruct(StructDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto nominal = mNominalTypes[identity];
    if (!nominal) {
        nominal = Type::makeStruct(decl->name, {}, identity);
        mNominalTypes[identity] = nominal;
    }
    nominal->typeParams = decl->typeParams;
    nominal->fields.clear();
    mNominalTypes[identity] = nominal;

    SymbolInfo info;
    info.kind = SymbolKind::Struct;
    info.isExported = decl->isExported;
    info.type = nominal;
    info.typeParams = decl->typeParams;
    if (!decl->versionTag) {
        mSymTable.defineAtRoot(decl->name, info);
        mSymTable.defineType(decl->name, nominal);
    }
    mSymTable.defineType(identity, nominal);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& field : decl->fields) {
        TypeField typedField;
        typedField.name = field.name;
        typedField.type = resolveTypeAST(field.type.get(), bindings);
        field.inferredType = typedField.type;
        nominal->fields.push_back(std::move(typedField));
    }
}

void SemanticAnalyzer::declareEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto nominal = mNominalTypes[identity];
    if (!nominal) {
        nominal = Type::makeEnum(decl->name, {}, identity);
        mNominalTypes[identity] = nominal;
    }
    nominal->typeParams = decl->typeParams;
    nominal->variants.clear();
    mNominalTypes[identity] = nominal;
    if (!decl->versionTag) mSymTable.defineType(decl->name, nominal);
    mSymTable.defineType(identity, nominal);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& variant : decl->variants) {
        TypeVariant typedVariant;
        typedVariant.name = variant.name;
        for (auto& field : variant.fields)
            typedVariant.fields.push_back(resolveTypeAST(field.get(), bindings));
        nominal->variants.push_back(std::move(typedVariant));
    }
}

void SemanticAnalyzer::declareTrait(TraitDecl* decl) {
    const std::string identity = traitIdentity(decl);
    if (decl->versionTag) {
        mVersionedTraits[decl->name][decl->versionTag->name].push_back(
            {decl->versionTag->version, decl});
    } else {
        mTraits[decl->name] = decl;
    }
    mTraitTypeParams[identity] = decl->typeParams;
    // Traits are compile-time interfaces. Their type object uses the complete
    // identity so tagged traits cannot be unified accidentally.
    mSymTable.defineType(identity, Type::makeTrait(identity));
    if (!decl->versionTag)
        mSymTable.defineType(decl->name, Type::makeTrait(identity));
}

void SemanticAnalyzer::declareImpl(ImplDecl* decl) {
    const std::string traitId = resolveTraitRef(decl->trait, decl);
    if (traitId.empty()) return;
    const TypePtr targetType = resolveTypeAST(decl->targetType.get(), {});
    const std::string targetId = typeIdentity(targetType);
    decl->resolvedTargetTypeId = targetId;
    auto& implementations = mImpls[traitId];
    if (implementations.count(targetId)) {
        error("duplicate impl of trait '" + traitId + "' for type '" + targetId + "'",
              decl->line, decl->col);
        return;
    }
    implementations.emplace(targetId, std::unordered_map<std::string, FunctionDecl*>{});
    auto& methods = implementations.at(targetId);
    for (auto& method : decl->methods) {
        if (methods.count(method->name)) {
            error("duplicate method '" + method->name + "' in impl of trait '" + traitId + "'",
                  method->line, method->col);
            continue;
        }
        methods[method->name] = method.get();
        method->generatedSymbolName = traitId + "__for__" + targetId + "__" + method->name;

        // Also register as callable function in symbol table
        SymbolInfo info;
        info.kind = SymbolKind::Function;
        info.returnType = declaredType(method->returnType.get(), {});
        method->inferredReturnType = info.returnType;
        for (auto& p : method->params) {
            p.inferredType = declaredType(p.type.get(), {});
            info.paramTypes.push_back(p.inferredType);
        }
        mSymTable.defineAtRoot(method->name, info);
    }
}

// ─── Analysis pass ─────────────────────────────────────────────────

void SemanticAnalyzer::analyzeFunction(FunctionDecl* decl) {
    TypePtr savedReturn = mCurrentReturnType;
    bool savedInFunction = mInFunction;
    bool savedInKernel = mInKernel;
    bool savedReturnsLinear = mCurrentFunctionReturnsLinear;
    bool savedSawReturn = mSawReturn;
    mInFunction = true;
    mInKernel = decl->isKernel;
    mCurrentFunctionReturnsLinear = decl->returnsLinear;
    mSawReturn = false;
    mSymTable.enterScope();
    enterConstScope();
    enterSlotScope();

    // Build type param bindings FIRST
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        TypePtr tpType = Type::makeTypeParam(tp);
        SymbolInfo info;
        info.kind = SymbolKind::TypeParam;
        info.type = tpType;
        mSymTable.define(tp, info);
        typeBindings[tp] = tpType;
    }

    mCurrentReturnType = decl->inferredReturnType
        ? decl->inferredReturnType
        : declaredType(decl->returnType.get(), typeBindings);

    if (decl->isKernel) {
        if (decl->isExtern || decl->isConstexpr || !decl->abi.empty())
            error("kernel '" + decl->name + "' cannot be extern, constexpr, or ABI-exported",
                  decl->line, decl->col);
        if (!decl->typeParams.empty())
            error("kernel '" + decl->name + "' cannot be generic in the initial device ABI",
                  decl->line, decl->col);
        if (decl->params.empty()) {
            error("kernel '" + decl->name + "' requires an explicit first parameter `index: i32`",
                  decl->line, decl->col);
        } else if (!decl->params.front().type ||
                   !resolved(decl->params.front().inferredType)->equals(TyI32)) {
            error("kernel '" + decl->name + "' must declare its first parameter as `index: i32`",
                  decl->line, decl->col);
        }
    }

    for (auto& p : decl->params) {
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = p.inferredType
            ? p.inferredType
            : declaredType(p.type.get(), typeBindings);
        info.isLinear = p.isLinear || dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr ||
                        resolved(info.type)->kind == TypeKind::DeviceBuffer ||
                        resolved(info.type)->kind == TypeKind::Event;
        p.inferredType = info.type;
        info.isHeapAllocated = false;
        mSymTable.define(p.name, info);
    }

    if (decl->isKernel) {
        for (size_t i = 1; i < decl->params.size(); ++i) {
            const auto& parameter = decl->params[i];
            const TypePtr type = resolved(parameter.inferredType);
            if (!parameter.type)
                error("kernel parameter '" + parameter.name + "' requires an explicit ABI type",
                      decl->line, decl->col);
            if (type->kind == TypeKind::DeviceBuffer)
                error("kernel parameter '" + parameter.name +
                      "' must use `&device_buffer<T>` or `&mut device_buffer<T>`; device buffers are never passed by value",
                      decl->line, decl->col);
            if (type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind != TypeKind::DeviceBuffer)
                error("kernel parameter '" + parameter.name +
                      "' may only use references to device_buffer<T> in the initial device ABI",
                      decl->line, decl->col);
        }
    }

    if (decl->body) {
        analyzeBlock(decl->body.get(), mCurrentReturnType);
    }

    // A function without an explicit return value is a unit-producing
    // declaration. Explicit return statements have already constrained this
    // variable through analyzeStmt.
    if (!decl->isExtern && !mSawReturn)
        constrain(mCurrentReturnType, TyUnit, "empty function body");

    // A single return statement is not enough for a non-unit function: every
    // reachable path must return a value.  Keeping this in semantic analysis
    // gives a source-level diagnostic instead of letting host IR verification
    // discover an unterminated LLVM basic block later in the pipeline.
    if (!decl->isExtern && decl->body &&
        !resolved(mCurrentReturnType)->equals(TyUnit) &&
        !blockAlwaysReturns(decl->body.get())) {
        error("function '" + decl->name + "' may finish without returning '" +
              resolved(mCurrentReturnType)->toString() + "'",
              decl->line, decl->col);
    }

    if (decl->isKernel && !resolved(mCurrentReturnType)->equals(TyUnit))
        error("kernel '" + decl->name + "' must return unit", decl->line, decl->col);

    mSymTable.exitScope();
    exitConstScope();
    exitSlotScope();
    mCurrentReturnType = savedReturn;
    mInFunction = savedInFunction;
    mInKernel = savedInKernel;
    mCurrentFunctionReturnsLinear = savedReturnsLinear;
    mSawReturn = savedSawReturn;
}

void SemanticAnalyzer::analyzeStruct(StructDecl* decl) {
    // Just validate fields don't have errors
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        typeBindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& field : decl->fields) {
        resolveTypeAST(field.type.get(), typeBindings);
    }
}

void SemanticAnalyzer::analyzeEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto it = mNominalTypes.find(identity);
    if (it == mNominalTypes.end()) return;
    std::unordered_map<std::string, bool> names;
    for (auto& variant : it->second->variants) {
        if (names[variant.name])
            error("Duplicate enum variant '" + variant.name + "' in '" + decl->name + "'");
        names[variant.name] = true;
    }
}

void SemanticAnalyzer::analyzeTrait(TraitDecl* decl) {
    // Store method signatures for constraint checking
    auto& sigs = mTraitMethods[traitIdentity(decl)];
    for (auto& method : decl->methods) {
        // Create a FunctionDecl-like entry
        auto fd = std::make_unique<FunctionDecl>();
        fd->name = method.name;
        fd->typeParams = decl->typeParams;
        for (auto& p : method.params) {
            Param paramClone;
            paramClone.name = p.name;
            paramClone.type = std::make_unique<NamedTypeAST>(
                p.type ? (dynamic_cast<NamedTypeAST*>(p.type.get()) ?
                         dynamic_cast<NamedTypeAST*>(p.type.get())->name : "?") : "?");
            fd->params.push_back(std::move(paramClone));
        }
        if (method.returnType) {
            fd->returnType = std::make_unique<NamedTypeAST>(
                dynamic_cast<NamedTypeAST*>(method.returnType.get()) ?
                dynamic_cast<NamedTypeAST*>(method.returnType.get())->name : "?");
        }
        sigs.emplace_back(method.name, fd.get());
        mGeneratedInstances.push_back(std::move(fd));
    }
}

void SemanticAnalyzer::analyzeImpl(ImplDecl* decl) {
    const std::string traitId = decl->trait.resolvedTraitId;
    if (traitId.empty()) return;
    // Verify that each impl method matches a trait method
    auto traitIt = mTraitMethods.find(traitId);
    if (traitIt == mTraitMethods.end()) {
        error("Impl for unknown trait '" + traitId + "'");
        return;
    }

    auto implementations = mImpls.find(traitId);
    auto implementation = implementations == mImpls.end()
        ? std::unordered_map<std::string, FunctionDecl*>{}
        : implementations->second[decl->resolvedTargetTypeId];
    std::unordered_map<std::string, bool> declaredMethods;
    for (const auto& [methodName, _] : traitIt->second) declaredMethods[methodName] = true;
    for (const auto& [methodName, _] : implementation) {
        if (!declaredMethods.count(methodName)) {
            error("impl of trait '" + displayTraitRef(decl->trait) +
                  "' defines unknown method '" + methodName + "'",
                  decl->line, decl->col);
        }
    }
    for (const auto& [methodName, _] : traitIt->second) {
        if (!implementation.count(methodName)) {
            error("impl of trait '" + displayTraitRef(decl->trait) +
                  "' for type '" + decl->resolvedTargetTypeId +
                  "' is missing method '" + methodName + "'", decl->line, decl->col);
        }
    }

    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        typeBindings[tp] = Type::makeTypeParam(tp);
    }

    for (auto& method : decl->methods) {
        // Analyze the method body
        analyzeFunction(method.get());
    }
}

void SemanticAnalyzer::analyzeSlotDecl(SlotDeclStmt* stmt) {
    if (mSlotScopes.back().count(stmt->name)) {
        error("duplicate slot declaration '" + stmt->name + "'", stmt->line, stmt->col);
        return;
    }
    TypeVec params;
    for (auto& param : stmt->params) {
        if (!param.type) {
            error("explicit slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
            params.push_back(TyUnknown);
        } else {
            param.inferredType = declaredType(param.type.get(), {});
            params.push_back(param.inferredType);
        }
    }
    SlotInfo info;
    info.name = stmt->name;
    info.acceptedKind = stmt->acceptedKind;
    info.acceptedCardinality = stmt->acceptedCardinality;
    info.isDynamic = stmt->isDynamic;
    info.paramTypes = params;
    for (const auto& param : stmt->params) info.paramNames.push_back(param.name);
    info.defaultFragment = stmt->defaultFragment;
    info.defaultFragmentSelector = stmt->defaultFragmentSelector;
    if (!info.defaultFragment.empty()) {
        if (auto* fragment = selectFragment(info.defaultFragment,
                                            info.defaultFragmentSelector, stmt)) {
            if (fragment->kind != info.acceptedKind ||
                fragment->cardinality != info.acceptedCardinality)
                error("default binding for slot '" + stmt->name +
                      "' has the wrong interceptor/context or once/many contract", stmt->line, stmt->col);
            info.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            stmt->resolvedDefaultFragmentName = info.resolvedDefaultFragmentName;
        }
    }
    info.structuralType = Type::makeSlot(
        params, TyUnit, info.acceptedCardinality == FragmentCardinality::Many,
        info.acceptedKind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context);
    stmt->structuralType = info.structuralType;
    mSlotScopes.back().emplace(stmt->name, info);

    SymbolInfo symbol;
    symbol.kind = SymbolKind::Slot;
    symbol.type = stmt->structuralType;
    if (!mSymTable.define(stmt->name, symbol))
        error("slot name '" + stmt->name + "' conflicts with an existing binding", stmt->line, stmt->col);
}

void SemanticAnalyzer::analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    if (stmt->isDynamic) {
        error("dynamic slot must be a separate declaration with an explicit interface; write `dynamic slot name(value: Type);`",
              stmt->line, stmt->col);
        return;
    }
    auto lookupSlot = [this](const std::string& name) -> SlotInfo* {
        for (auto it = mSlotScopes.rbegin(); it != mSlotScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };
    auto lookupApplied = [this](const std::string& name) -> FragmentDecl* {
        for (auto it = mApplyScopes.rbegin(); it != mApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    };
    auto lookupDynamicApplied = [this](const std::string& name) -> const std::vector<FragmentDecl*>* {
        for (auto it = mDynamicApplyScopes.rbegin(); it != mDynamicApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };

    SlotInfo active;
    if (stmt->isImplicitCapture) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.isImplicitCapture = true;
        active.defaultFragment = stmt->defaultFragment;
        active.defaultFragmentSelector = stmt->defaultFragmentSelector;
        active.structuralType = Type::makeSlot(
            {}, TyUnit, active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context);
        if (!mSlotScopes.back().emplace(stmt->name, active).second) {
            error("duplicate implicit slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = mSlotScopes.back().at(stmt->name);
    } else if (!stmt->interfaceParams.empty()) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.defaultFragment = stmt->defaultFragment;
        active.defaultFragmentSelector = stmt->defaultFragmentSelector;
        for (auto& param : stmt->interfaceParams) {
            if (!param.type) {
                error("inline slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
                active.paramTypes.push_back(TyUnknown);
                continue;
            }
            auto parameterType = declaredType(param.type.get(), {});
            param.inferredType = parameterType;
            active.paramTypes.push_back(parameterType);
            active.paramNames.push_back(param.name);
            auto* captured = mSymTable.lookup(param.name);
            if (!captured) error("inline slot parameter '" + param.name + "' has no matching local binding");
            else constrain(captured->type, parameterType,
                           "inline slot parameter '" + param.name + "'");
        }
        active.structuralType = Type::makeSlot(
            active.paramTypes, TyUnit,
            active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context);
        mSlotScopes.back()[stmt->name] = active;
    } else {
        auto* declared = lookupSlot(stmt->name);
        if (!declared) {
            error("unknown slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = *declared;
        stmt->acceptedKind = active.acceptedKind;
        stmt->acceptedCardinality = active.acceptedCardinality;
        if (stmt->args.size() != active.paramTypes.size()) {
            error("slot '" + stmt->name + "' expects " +
                  std::to_string(active.paramTypes.size()) + " arguments, got " +
                  std::to_string(stmt->args.size()), stmt->line, stmt->col);
        }
        const size_t count = std::min(stmt->args.size(), active.paramTypes.size());
        for (size_t i = 0; i < count; ++i)
            constrain(analyzeExpr(stmt->args[i].get()), active.paramTypes[i],
                      "argument " + std::to_string(i + 1) + " of slot '" + stmt->name + "'");
    }

    stmt->structuralType = active.structuralType;
    stmt->resolvedParamNames = active.paramNames;
    const auto captures = mSymTable.visibleSymbols();
    analyzeBlock(stmt->continuation.get(), expectedReturn);

    FragmentDecl* fragment = lookupApplied(stmt->name);
    const std::vector<FragmentDecl*>* dynamicFragments = lookupDynamicApplied(stmt->name);
    if (dynamicFragments && !dynamicFragments->empty()) {
        stmt->usesDynamicDispatch = true;
        stmt->resolvedDynamicFragmentNames.clear();
        FragmentDecl* contract = nullptr;
        for (auto* candidate : *dynamicFragments) {
            if (!candidate) continue;
            if (candidate->cardinality == FragmentCardinality::Many)
                error("dynamic apply currently rejects explicitly multi-shot context '" +
                      candidate->name + "'", stmt->line, stmt->col);
            if (candidate->kind != active.acceptedKind ||
                candidate->cardinality != active.acceptedCardinality)
                error("dynamic candidate '" + candidate->name +
                      "' does not match slot '" + stmt->name + "' contract",
                      stmt->line, stmt->col);
            if (!contract) contract = candidate;
            else if (candidate->kind != contract->kind ||
                     candidate->cardinality != contract->cardinality) {
                error("all dynamic candidates must declare the same interceptor/context and once/many contract",
                      stmt->line, stmt->col);
            }
            stmt->resolvedDynamicFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
            analyzeFragmentForSlot(candidate, stmt->name, active.paramTypes, captures);
        }
        return;
    }
    if (!fragment && !active.defaultFragment.empty()) {
        fragment = selectFragment(active.defaultFragment, active.defaultFragmentSelector, stmt);
    }
    if (fragment) {
        active.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
        stmt->resolvedDefaultFragmentName = active.resolvedDefaultFragmentName;
    }
    if (!fragment) return; // no binding is an identity fragment: resume once
    if (fragment->kind != active.acceptedKind ||
        fragment->cardinality != active.acceptedCardinality)
        error("fragment '" + fragment->name + "' does not match slot '" + stmt->name +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    analyzeFragmentForSlot(fragment, stmt->name, active.paramTypes, captures);
}

void SemanticAnalyzer::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    auto* fragment = selectFragment(stmt->fragmentName, stmt->fragmentSelector, stmt);
    if (!fragment) return;
    stmt->resolvedFragmentName = fragment->generatedSymbolName.empty()
        ? fragment->name : fragment->generatedSymbolName;
    SlotInfo* knownSlot = nullptr;
    for (auto it = mSlotScopes.rbegin(); it != mSlotScopes.rend(); ++it) {
        auto found = it->find(stmt->slotName);
        if (found != it->end()) { knownSlot = &found->second; break; }
    }
    auto matchesContract = [](const FragmentDecl* candidate, const SlotInfo* slot) {
        return !slot || (candidate->kind == slot->acceptedKind &&
                         candidate->cardinality == slot->acceptedCardinality);
    };
    if (!matchesContract(fragment, knownSlot))
        error("fragment '" + fragment->name + "' does not match slot '" + stmt->slotName +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    if (stmt->isDynamic) {
        SlotInfo* slot = knownSlot;
        if (!slot) {
            error("dynamic apply requires a previously declared dynamic slot '" + stmt->slotName + "'",
                  stmt->line, stmt->col);
            return;
        }
        if (!slot->isDynamic) {
            error("slot '" + stmt->slotName + "' is static; declare it with `dynamic slot` before dynamic apply",
                  stmt->line, stmt->col);
            return;
        }
        std::vector<FragmentDecl*> candidates{fragment};
        for (const auto& name : stmt->alternativeFragmentNames) {
            auto* candidate = selectFragment(name, std::nullopt, stmt);
            if (!candidate) continue;
            if (!matchesContract(candidate, slot))
                error("dynamic candidate '" + candidate->name + "' does not match slot '" +
                      stmt->slotName + "' contract", stmt->line, stmt->col);
            candidates.push_back(candidate);
            stmt->resolvedAlternativeFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
        }
        if (stmt->body) {
            enterSlotScope();
            mApplyScopes.back()[stmt->slotName] = fragment;
            mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
            analyzeBlock(stmt->body.get(), expectedReturn);
            exitSlotScope();
        } else {
            mApplyScopes.back()[stmt->slotName] = fragment;
            mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
        }
        return;
    }
    if (stmt->body) {
        enterSlotScope();
        mApplyScopes.back()[stmt->slotName] = fragment;
        analyzeBlock(stmt->body.get(), expectedReturn);
        exitSlotScope();
    } else {
        mApplyScopes.back()[stmt->slotName] = fragment;
    }
}

void SemanticAnalyzer::analyzeFragmentForSlot(
    FragmentDecl* fragment, const std::string& slotName, const TypeVec& parameterTypes,
    const std::unordered_map<std::string, SymbolInfo>& captures) {
    if (fragment->params.size() > parameterTypes.size()) {
        error("fragment '" + fragment->name + "' requires " +
              std::to_string(fragment->params.size()) + " parameters, but slot '" + slotName +
              " provides " + std::to_string(parameterTypes.size()));
        return;
    }

    SlotInfo context;
    context.name = slotName;
    context.paramTypes = parameterTypes;
    context.structuralType = Type::makeSlot(
        parameterTypes, TyUnit,
        fragment->cardinality == FragmentCardinality::Many,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context);
    const SlotInfo* savedSlot = mCurrentFragmentSlot;
    FragmentDecl* savedFragment = mCurrentFragmentDecl;
    TypePtr savedReturnType = mCurrentReturnType;
    mCurrentFragmentSlot = &context;
    mCurrentFragmentDecl = fragment;
    mCurrentReturnType = TyUnit;

    mSymTable.enterScope();
    for (size_t i = 0; i < fragment->params.size(); ++i) {
        auto& param = fragment->params[i];
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = parameterTypes[i];
        if (param.type) constrain(declaredType(param.type.get(), {}), info.type,
                                  "parameter '" + param.name + "' of fragment '" + fragment->name + "'");
        param.inferredType = info.type;
        mSymTable.define(param.name, info);
    }
    for (const auto& [name, info] : captures) {
        if (!mSymTable.hasInCurrentScope(name)) mSymTable.define(name, info);
    }
    analyzeBlock(fragment->body.get(), TyUnit);
    mSymTable.exitScope();
    mCurrentReturnType = savedReturnType;

    struct ControlPaths {
        std::set<int> active{0};
        bool aborted = false;
        bool returned = false;
        bool abortAfterResume = false;
    };
    std::function<ControlPaths(const BlockStmt*, const std::set<int>&)> analyzePaths;
    std::function<ControlPaths(const Stmt*, const std::set<int>&)> analyzeStmtPaths;
    auto mergePaths = [](ControlPaths left, const ControlPaths& right) {
        left.active.insert(right.active.begin(), right.active.end());
        left.aborted = left.aborted || right.aborted;
        left.returned = left.returned || right.returned;
        left.abortAfterResume = left.abortAfterResume || right.abortAfterResume;
        return left;
    };
    analyzeStmtPaths = [&](const Stmt* stmt, const std::set<int>& incoming) -> ControlPaths {
        ControlPaths out; out.active = incoming;
        if (!stmt) return out;
        if (dynamic_cast<const ResumeStmt*>(stmt)) {
            out.active.clear();
            for (int count : incoming) out.active.insert(std::min(count + 1, 2));
            return out;
        }
        if (dynamic_cast<const AbortStmt*>(stmt)) {
            for (int count : incoming) if (count > 0) out.abortAfterResume = true;
            out.active.clear(); out.aborted = true; return out;
        }
        if (dynamic_cast<const ReturnStmt*>(stmt)) {
            out.active.clear(); out.returned = true; return out;
        }
        if (auto* block = dynamic_cast<const BlockStmt*>(stmt))
            return analyzePaths(block, incoming);
        if (auto* branch = dynamic_cast<const IfStmt*>(stmt)) {
            ControlPaths thenPaths = analyzePaths(branch->thenBlock.get(), incoming);
            ControlPaths elsePaths;
            elsePaths.active = incoming;
            if (branch->elseBranch) elsePaths = analyzeStmtPaths(branch->elseBranch.get(), incoming);
            return mergePaths(std::move(thenPaths), elsePaths);
        }
        if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            // A loop may execute zero times or repeat. Any resume in its body
            // therefore makes a once-context path potentially multi-shot.
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        return out;
    };
    analyzePaths = [&](const BlockStmt* block, const std::set<int>& incoming) {
        ControlPaths paths; paths.active = incoming;
        if (!block) return paths;
        for (const auto& statement : block->stmts) {
            if (paths.active.empty()) break;
            ControlPaths next = analyzeStmtPaths(statement.get(), paths.active);
            paths.active = std::move(next.active);
            paths.aborted = paths.aborted || next.aborted;
            paths.returned = paths.returned || next.returned;
            paths.abortAfterResume = paths.abortAfterResume || next.abortAfterResume;
        }
        return paths;
    };
    const ControlPaths control = analyzePaths(fragment->body.get(), {0});
    const bool isMany = fragment->cardinality == FragmentCardinality::Many;
    if (!isMany && control.abortAfterResume)
        error("single-shot context '" + fragment->name +
              "' cannot abort after resume(); the continuation has already been consumed");
    if (fragment->kind == FragmentKind::Context) {
        for (int resumes : control.active) {
            if (!isMany && resumes > 1) {
                error("single-shot context '" + fragment->name +
                      "' may resume its continuation at most once; a path with no `resume()` is an implicit abort");
                break;
            }
        }
    }
    fragment->structuralType = Type::makeFragment(
        parameterTypes, TyUnit, isMany,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context);
    if (isMany) {
        for (const auto& [name, info] : captures) {
            if (info.isLinear) {
                error("fragment '" + fragment->name + "' may resume slot '" + slotName +
                      "' more than once, but captured linear value '" + name + "' is not replayable");
            }
        }
    }

    mCurrentFragmentSlot = savedSlot;
    mCurrentFragmentDecl = savedFragment;
}

void SemanticAnalyzer::enterSlotScope() {
    mSlotScopes.emplace_back();
    mApplyScopes.emplace_back();
    mDynamicApplyScopes.emplace_back();
}

void SemanticAnalyzer::exitSlotScope() {
    if (mSlotScopes.size() > 1) mSlotScopes.pop_back();
    if (mApplyScopes.size() > 1) mApplyScopes.pop_back();
    if (mDynamicApplyScopes.size() > 1) mDynamicApplyScopes.pop_back();
}

FunctionDecl* SemanticAnalyzer::selectVersionedFunction(
    const std::string& name, const VersionSelector& selector, const ASTNode* useSite) {
    auto functions = mVersionedFunctions.find(name);
    if (functions == mVersionedFunctions.end()) {
        error("function '" + name + "' has no versioned declarations", useSite->line, useSite->col);
        return nullptr;
    }
    auto tagged = functions->second.find(selector.tag);
    if (tagged == functions->second.end() || tagged->second.empty()) {
        error("function '" + name + "' has no `@" + selector.tag + "` version", useSite->line, useSite->col);
        return nullptr;
    }
    FunctionDecl* selected = nullptr;
    for (auto* candidate : tagged->second) {
        if (!candidate || !candidate->versionTag) continue;
        if (selector.version && !(candidate->versionTag->version == *selector.version)) continue;
        if (!selected || selected->versionTag->version < candidate->versionTag->version)
            selected = candidate;
    }
    if (!selected) {
        error("function '" + name + "' has no `@" + selector.tag + "(" +
              selector.version->toString() + ")` declaration", useSite->line, useSite->col);
    }
    return selected;
}

FragmentDecl* SemanticAnalyzer::selectFragment(
    const std::string& name, const std::optional<VersionSelector>& selector,
    const ASTNode* useSite) {
    if (!selector) {
        auto fragment = mFragments.find(name);
        if (fragment != mFragments.end()) return fragment->second;
        error("unknown fragment '" + name + "'", useSite->line, useSite->col);
        return nullptr;
    }
    auto fragments = mVersionedFragments.find(name);
    if (fragments == mVersionedFragments.end()) {
        error("fragment '" + name + "' has no versioned declarations", useSite->line, useSite->col);
        return nullptr;
    }
    auto tagged = fragments->second.find(selector->tag);
    if (tagged == fragments->second.end() || tagged->second.empty()) {
        error("fragment '" + name + "' has no `@" + selector->tag + "` version",
              useSite->line, useSite->col);
        return nullptr;
    }
    FragmentDecl* selected = nullptr;
    for (auto* candidate : tagged->second) {
        if (!candidate || !candidate->versionTag) continue;
        if (selector->version && !(candidate->versionTag->version == *selector->version)) continue;
        if (!selected || selected->versionTag->version < candidate->versionTag->version)
            selected = candidate;
    }
    if (!selected) {
        error("fragment '" + name + "' has no `@" + selector->tag + "(" +
              selector->version->toString() + ")` declaration", useSite->line, useSite->col);
    }
    return selected;
}

TypePtr SemanticAnalyzer::selectVersionedNominal(
    const std::string& name, const VersionSelector& selector, const ASTNode* useSite) {
    auto types = mVersionedNominals.find(name);
    if (types == mVersionedNominals.end()) {
        error("type '" + name + "' has no versioned declarations", useSite->line, useSite->col);
        return TyUnknown;
    }
    auto tagged = types->second.find(selector.tag);
    if (tagged == types->second.end() || tagged->second.empty()) {
        error("type '" + name + "' has no `@" + selector.tag + "` version",
              useSite->line, useSite->col);
        return TyUnknown;
    }
    const VersionedNominal* selected = nullptr;
    for (const auto& candidate : tagged->second) {
        if (selector.version && !(candidate.version == *selector.version)) continue;
        if (!selected || selected->version < candidate.version) selected = &candidate;
    }
    if (!selected) {
        error("type '" + name + "' has no `@" + selector.tag + "(" +
              selector.version->toString() + ")` declaration", useSite->line, useSite->col);
        return TyUnknown;
    }
    return selected->type;
}

TraitDecl* SemanticAnalyzer::selectVersionedTrait(
    const std::string& name, const VersionSelector& selector, const ASTNode* useSite) {
    auto traits = mVersionedTraits.find(name);
    if (traits == mVersionedTraits.end()) {
        error("trait '" + name + "' has no versioned declarations", useSite->line, useSite->col);
        return nullptr;
    }
    auto tagged = traits->second.find(selector.tag);
    if (tagged == traits->second.end() || tagged->second.empty()) {
        error("trait '" + name + "' has no `@" + selector.tag + "` version",
              useSite->line, useSite->col);
        return nullptr;
    }
    TraitDecl* selected = nullptr;
    for (const auto& candidate : tagged->second) {
        if (!candidate.decl) continue;
        if (selector.version && !(candidate.version == *selector.version)) continue;
        if (!selected || selected->versionTag->version < candidate.version)
            selected = candidate.decl;
    }
    if (!selected) {
        error("trait '" + name + "' has no `@" + selector.tag + "(" +
              selector.version->toString() + ")` declaration", useSite->line, useSite->col);
    }
    return selected;
}

std::string SemanticAnalyzer::traitIdentity(const TraitDecl* trait) const {
    if (!trait) return "";
    return trait->generatedSymbolName.empty()
        ? trait->name : trait->generatedSymbolName;
}

std::string SemanticAnalyzer::typeIdentity(const TypePtr& type) const {
    if (!type) return "?";
    if (!type->nominalId.empty()) return type->nominalId;
    return type->toString();
}

bool SemanticAnalyzer::satisfiesTrait(const std::string& traitId, const TypePtr& type) const {
    const auto trait = mImpls.find(traitId);
    if (trait == mImpls.end()) return false;
    const auto implementation = trait->second.find(typeIdentity(type));
    if (implementation == trait->second.end()) return false;
    const auto signatures = mTraitMethods.find(traitId);
    if (signatures == mTraitMethods.end()) return false;
    for (const auto& [methodName, _] : signatures->second) {
        if (!implementation->second.count(methodName)) return false;
    }
    return true;
}

std::string SemanticAnalyzer::resolveTraitRef(TraitRef& trait, const ASTNode* useSite) {
    if (!trait.resolvedTraitId.empty()) return trait.resolvedTraitId;
    const ASTNode* diagnosticSite = trait.line > 0 ? static_cast<const ASTNode*>(&trait) : useSite;
    TraitDecl* selected = nullptr;
    if (trait.versionSelector) {
        selected = selectVersionedTrait(trait.name, *trait.versionSelector, diagnosticSite);
    } else {
        auto untagged = mTraits.find(trait.name);
        if (untagged != mTraits.end()) {
            selected = untagged->second;
        } else if (mVersionedTraits.count(trait.name)) {
            error("trait '" + trait.name + "' is versioned; select a tag explicitly",
                  diagnosticSite->line, diagnosticSite->col);
            return "";
        } else {
            error("unknown trait '" + trait.name + "'", diagnosticSite->line, diagnosticSite->col);
            return "";
        }
    }
    if (!selected) return "";
    trait.resolvedTraitId = traitIdentity(selected);
    return trait.resolvedTraitId;
}

// ─── Statement analysis ────────────────────────────────────────────

TypePtr SemanticAnalyzer::analyzeStmt(Stmt* stmt, TypePtr expectedReturn) {
    setDiagnosticLocation(stmt);
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) return analyzeBlock(bs, expectedReturn);

    // A device kernel is deliberately a small DeviceMemory-only sublanguage.
    // The source-level continuation constructs are host control flow: lowering
    // them into SIMT code would require a per-lane continuation runtime and
    // could not preserve their resume semantics. Likewise, awaiting, freeing,
    // and host allocation are host-side synchronization/resource effects.
    // Keep this boundary in semantic analysis, before code generation can
    // accidentally inline a fragment or emit a host runtime call into HSACO.
    if (mInKernel) {
        const char* construct = nullptr;
        if (dynamic_cast<SlotDeclStmt*>(stmt)) construct = "slot declaration";
        else if (dynamic_cast<SlotInvokeStmt*>(stmt)) construct = "slot invocation";
        else if (dynamic_cast<ApplyStmt*>(stmt)) construct = "apply binding";
        else if (dynamic_cast<ResumeStmt*>(stmt)) construct = "resume()";
        else if (dynamic_cast<AbortStmt*>(stmt)) construct = "abort()";
        else if (dynamic_cast<AwaitStmt*>(stmt)) construct = "await";
        else if (dynamic_cast<FreeStmt*>(stmt)) construct = "free";
        if (construct) {
            error("kernel body may not use " + std::string(construct) +
                  "; device kernels support only DeviceMemory operations and structured scalar control flow",
                  stmt->line, stmt->col);
            return TyUnit;
        }
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        analyzeSlotDecl(slot);
        return TyUnit;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        analyzeSlotInvoke(slot, expectedReturn);
        return TyUnit;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        analyzeApply(apply, expectedReturn);
        return TyUnit;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mCurrentFragmentDecl)
            error("`resume()` may only appear inside a fragment", stmt->line, stmt->col);
        else if (mCurrentFragmentDecl &&
                 mCurrentFragmentDecl->kind == FragmentKind::Interceptor)
            error("`resume()` is not allowed in an interceptor; normal completion forwards automatically",
                  stmt->line, stmt->col);
        return TyUnit;
    }
    if (dynamic_cast<AbortStmt*>(stmt)) {
        if (!mCurrentFragmentDecl)
            error("`abort()` may only appear inside an interceptor or context", stmt->line, stmt->col);
        return TyUnit;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        TypePtr eventType = resolved(analyzeExpr(await->event.get()));
        if (eventType->kind != TypeKind::Event)
            error("`await` requires a launch event, got " + eventType->toString(),
                  await->line, await->col);
        return TyUnit;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt)) {
        TypePtr rhsType = analyzeExpr(ls->initializer.get());

        // Check if rhs is a HeapAllocExpr — mark as heap allocated
        bool isHeap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()) != nullptr;

        TypePtr declaredType;
        if (ls->typeAnnotation) {
            std::unordered_map<std::string, TypePtr> bindings;
            declaredType = this->declaredType(ls->typeAnnotation.get(), bindings);
            if (!(dynamic_cast<StringLiteralExpr*>(ls->initializer.get()) &&
                  resolved(declaredType)->kind == TypeKind::CStr))
                constrain(rhsType, declaredType, "let binding '" + ls->name + "'");
        } else {
            declaredType = rhsType; // auto inference
        }

        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = declaredType;
        info.isConst = ls->isConst;
        info.isHeapAllocated = isHeap || declaredType->isHeapType();
        auto finalType = resolved(declaredType);
        info.isLinear = ls->isLinear || dynamic_cast<LinearTypeAST*>(ls->typeAnnotation.get()) != nullptr ||
                        finalType->kind == TypeKind::DeviceBuffer || finalType->kind == TypeKind::Event;
        mSymTable.define(ls->name, info);
        // Device resources and completion events carry ownership semantics.
        // Preserve their inferred type on the AST so the ownership and codegen
        // passes observe the same ABI even without an explicit annotation.
        if (!ls->typeAnnotation &&
            (finalType->kind == TypeKind::DeviceBuffer || finalType->kind == TypeKind::Event ||
             finalType->kind == TypeKind::Array || finalType->kind == TypeKind::Slice))
            ls->typeAnnotation = typeToAST(finalType);
        if (ls->isConst) {
            auto value = evaluateConstExpr(ls->initializer.get());
            if (!value) {
                error("const binding '" + ls->name + "' is not a compile-time expression");
            } else {
                defineConst(ls->name, *value);
            }
        }
        return TyUnit;
    }
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        mSawReturn = true;
        if (rs->value) {
            TypePtr valueType = analyzeExpr(rs->value.get());
            if (!(dynamic_cast<StringLiteralExpr*>(rs->value.get()) &&
                  resolved(mCurrentReturnType)->kind == TypeKind::CStr))
                constrain(valueType, mCurrentReturnType, "return statement");
            bool returningLinear = false;
            if (auto* call = dynamic_cast<CallExpr*>(rs->value.get())) {
                returningLinear = call->returnsLinear;
            } else if (auto* id = dynamic_cast<IdentifierExpr*>(rs->value.get())) {
                if (auto* symbol = mSymTable.lookup(id->name))
                    returningLinear = symbol->isLinear;
            }
            if (returningLinear && !mCurrentFunctionReturnsLinear) {
                error("returning an owning value requires a `-> linear raw<T>` function return annotation",
                      rs->line, rs->col);
            } else if (!returningLinear && mCurrentFunctionReturnsLinear) {
                error("function declared with `-> linear raw<T>` must return an owning value",
                      rs->line, rs->col);
            }
        } else {
            constrain(TyUnit, mCurrentReturnType, "unit return statement");
        }
        return TyUnit;
    }
    if (auto* is = dynamic_cast<IfStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(is->cond.get());
        requireBool(condType, "if condition");
        analyzeBlock(is->thenBlock.get(), expectedReturn);
        if (is->elseBranch) analyzeStmt(is->elseBranch.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(ws->cond.get());
        requireBool(condType, "while condition");
        analyzeBlock(ws->body.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt)) {
        // For simplicity, just analyze body; iterable type isn't checked deeply
        analyzeExpr(fs->iterable.get());
        mSymTable.enterScope();
        SymbolInfo vi;
        vi.kind = SymbolKind::Variable;
        vi.type = TyI32; // assume iterator yields i32 for now
        mSymTable.define(fs->varName, vi);
        analyzeBlock(fs->body.get(), expectedReturn);
        mSymTable.exitScope();
        return TyUnit;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) {
        return analyzeExpr(es->expr.get());
    }
    if (auto* fs = dynamic_cast<FreeStmt*>(stmt)) {
        analyzeExpr(fs->operand.get());
        return TyUnit;
    }
    return TyUnit;
}

TypePtr SemanticAnalyzer::analyzeBlock(BlockStmt* block, TypePtr expectedReturn) {
    mSymTable.enterScope();
    enterConstScope();
    enterSlotScope();
    for (auto& stmt : block->stmts) {
        analyzeStmt(stmt.get(), expectedReturn);
    }
    mSymTable.exitScope();
    exitConstScope();
    exitSlotScope();
    return TyUnit;
}

bool SemanticAnalyzer::statementAlwaysReturns(const Stmt* stmt) const {
    if (!stmt) return false;
    if (dynamic_cast<const ReturnStmt*>(stmt)) return true;
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) return blockAlwaysReturns(block);
    if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        return conditional->elseBranch &&
               blockAlwaysReturns(conditional->thenBlock.get()) &&
               statementAlwaysReturns(conditional->elseBranch.get());
    }
    if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt))
        return apply->body && blockAlwaysReturns(apply->body.get());
    return false;
}

bool SemanticAnalyzer::blockAlwaysReturns(const BlockStmt* block) const {
    if (!block) return false;
    for (const auto& stmt : block->stmts) {
        if (statementAlwaysReturns(stmt.get())) return true;
    }
    return false;
}

// ─── Expression analysis ───────────────────────────────────────────

TypePtr SemanticAnalyzer::analyzeExpr(Expr* expr) {
    setDiagnosticLocation(expr);
    if (auto* il = dynamic_cast<IntLiteralExpr*>(expr)) return TyI32;
    if (auto* fl = dynamic_cast<FloatLiteralExpr*>(expr)) return TyF64;
    if (auto* sl = dynamic_cast<StringLiteralExpr*>(expr)) return TyString;
    if (auto* bl = dynamic_cast<BoolLiteralExpr*>(expr)) return TyBool;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto* sym = mSymTable.lookup(id->name);
        if (!sym) {
            error("undefined name '" + id->name + "'", id->line, id->col);
            return TyUnknown;
        }
        if (sym->kind == SymbolKind::Function)
            return Type::makeFunction(sym->paramTypes,
                                      sym->returnType ? sym->returnType : TyUnit);
        return sym->type ? resolved(sym->type) : TyUnknown;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        TypePtr lhsType = analyzeExpr(bin->lhs.get());
        TypePtr rhsType = analyzeExpr(bin->rhs.get());
        switch (bin->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                requireNumeric(lhsType, "left operand of arithmetic expression");
                requireNumeric(rhsType, "right operand of arithmetic expression");
                constrain(lhsType, rhsType, "arithmetic operands");
                return resolved(lhsType);
            case TokenKind::Ampersand:
            case TokenKind::BitOr:
            case TokenKind::BitXor:
                requireInteger(lhsType, "left operand of bitwise expression");
                requireInteger(rhsType, "right operand of bitwise expression");
                if ((resolved(lhsType)->kind == TypeKind::InferenceVar || isIntegerType(resolved(lhsType))) &&
                    (resolved(rhsType)->kind == TypeKind::InferenceVar || isIntegerType(resolved(rhsType))))
                    constrain(lhsType, rhsType, "bitwise operands");
                return resolved(lhsType);
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                requireInteger(lhsType, "left operand of shift expression");
                requireInteger(rhsType, "shift count");
                return resolved(lhsType);
            case TokenKind::EqEq:
            case TokenKind::Neq:
                constrain(lhsType, rhsType, "equality operands");
                return TyBool;
            case TokenKind::Lt:
            case TokenKind::LtEq:
            case TokenKind::Gt:
            case TokenKind::GtEq:
                requireNumeric(lhsType, "left operand of comparison expression");
                requireNumeric(rhsType, "right operand of comparison expression");
                constrain(lhsType, rhsType, "comparison operands");
                return TyBool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                requireBool(lhsType, "left operand of logical expression");
                requireBool(rhsType, "right operand of logical expression");
                return TyBool;
            default: return TyUnknown;
        }
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        TypePtr opType = analyzeExpr(un->operand.get());
        switch (un->op) {
            case TokenKind::Minus:
                requireNumeric(opType, "unary '-' operand");
                return opType;
            case TokenKind::Not:
                requireBool(opType, "'!' operand");
                return TyBool;
            case TokenKind::Tilde:
                requireInteger(opType, "'~' operand");
                return opType;
            case TokenKind::Star: {
                auto resolvedOp = resolved(opType);
                if (resolvedOp->kind == TypeKind::InferenceVar) {
                    auto inner = mConstraints.fresh();
                    constrain(opType, Type::makeReference(inner), "dereference operand");
                    return inner;
                }
                if (resolvedOp->kind == TypeKind::Reference) return resolvedOp->inner;
                error("Cannot dereference non-reference type");
                return TyUnknown;
            }
            default: return TyUnknown;
        }
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr)) {
        auto nominalIt = mNominalTypes.find(variant->typeName);
        if (nominalIt == mNominalTypes.end() ||
            nominalIt->second->kind != TypeKind::Enum) {
            error("'" + variant->typeName + "' is not an enum type");
            return TyUnknown;
        }

        TypeVec typeArgs;
        for (auto& arg : variant->typeArgs)
            typeArgs.push_back(resolveTypeAST(arg.get(), {}));
        if (typeArgs.empty()) {
            for (size_t i = 0; i < nominalIt->second->typeParams.size(); ++i)
                typeArgs.push_back(mConstraints.fresh());
        } else if (typeArgs.size() != nominalIt->second->typeParams.size()) {
            error("Enum '" + variant->typeName + "' expects " +
                  std::to_string(nominalIt->second->typeParams.size()) +
                  " type arguments");
        }

        auto constructed = instantiateNominal(nominalIt->second, typeArgs);
        variant->constructedType = constructed;
        mInferenceRoots.emplace_back(constructed,
                                     "type arguments of '" + variant->typeName +
                                     "::" + variant->variantName + "'");
        const TypeVariant* selected = nullptr;
        for (auto& candidate : constructed->variants) {
            if (candidate.name == variant->variantName) {
                selected = &candidate;
                break;
            }
        }
        if (!selected) {
            error("Enum '" + variant->typeName + "' has no variant '" +
                  variant->variantName + "'");
            return TyUnknown;
        }
        if (selected->fields.size() != variant->args.size()) {
            error("Variant '" + variant->variantName + "' expects " +
                  std::to_string(selected->fields.size()) + " arguments");
            return constructed;
        }
        for (size_t i = 0; i < variant->args.size(); ++i)
            constrain(analyzeExpr(variant->args[i].get()), selected->fields[i],
                      "enum variant argument");
        return constructed;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return analyzeLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr)) return analyzeCall(call);
    if (auto* fa = dynamic_cast<FieldAccessExpr*>(expr)) {
        TypePtr objectType = resolved(analyzeExpr(fa->object.get()));
        if (objectType->kind == TypeKind::Reference && objectType->inner)
            objectType = resolved(objectType->inner);
        if (objectType->kind != TypeKind::Struct && objectType->kind != TypeKind::Record) {
            error("Field access requires a product type, got " + objectType->toString());
            return TyUnknown;
        }
        for (auto& field : objectType->fields) {
            if (field.name == fa->field) return field.type;
        }
        error("Type '" + objectType->toString() + "' has no field '" + fa->field + "'");
        return TyUnknown;
    }
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr)) {
        if (mInKernel) {
            error("kernel body may not allocate heap memory with `new`; allocate device memory on the host and pass a borrowed device_buffer parameter",
                  ha->line, ha->col);
            return TyUnknown;
        }
        ha->allocatedType = ha->allocatedTypeAST
            ? resolveTypeAST(ha->allocatedTypeAST.get(), {}) : TyUnknown;
        if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
            TypeVec argumentTypes;
            for (auto& arg : initCall->args)
                argumentTypes.push_back(analyzeExpr(arg.get()));
            if (ha->allocatedType->kind == TypeKind::Struct ||
                ha->allocatedType->kind == TypeKind::Record) {
                if (initCall->args.size() != ha->allocatedType->fields.size()) {
                    error("Constructor for '" + ha->allocatedType->toString() +
                          "' expects " + std::to_string(ha->allocatedType->fields.size()) +
                          " field values");
                } else {
                    for (size_t i = 0; i < initCall->args.size(); ++i)
                        constrain(argumentTypes[i],
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
                    error("Primitive allocation requires exactly one initializer");
                else
                    constrain(argumentTypes[0], ha->allocatedType,
                              "primitive allocation initializer");
            }
        }
        return ha->allocatedType;
    }
    if (auto* mv = dynamic_cast<MoveExpr*>(expr)) {
        return analyzeExpr(mv->operand.get());
    }
    if (auto* bw = dynamic_cast<BorrowExpr*>(expr)) {
        TypePtr inner = analyzeExpr(bw->operand.get());
        return Type::makeReference(inner, bw->isMutable);
    }
    if (auto* dr = dynamic_cast<DerefExpr*>(expr)) {
        TypePtr op = resolved(analyzeExpr(dr->operand.get()));
        if (op->kind == TypeKind::Reference && op->inner) return op->inner;
        error("Cannot dereference non-reference type");
        return TyUnknown;
    }
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr)) {
        TypePtr op = analyzeExpr(ad->operand.get());
        return Type::makeReference(op, ad->isMutable);
    }
    if (auto* le = dynamic_cast<LambdaExpr*>(expr)) {
        // Analyze lambda: enter new scope, register params, analyze body
        TypePtr savedReturn = mCurrentReturnType;
        bool savedSawReturn = mSawReturn;
        mSymTable.enterScope();
        for (auto& p : le->params) {
            TypePtr pt = p.inferredType ? p.inferredType : declaredType(p.type.get(), {});
            p.inferredType = pt;
            SymbolInfo info;
            info.kind = SymbolKind::Variable;
            info.type = pt;
            info.isLinear = p.isLinear || dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr;
            mSymTable.define(p.name, info);
        }
        TypePtr bodyRet = le->returnType ? declaredType(le->returnType.get(), {}) : mConstraints.fresh();
        mCurrentReturnType = bodyRet;
        mSawReturn = false;
        if (le->body) analyzeBlock(le->body.get(), bodyRet);
        if (!mSawReturn) constrain(bodyRet, TyUnit, "lambda without a return value");
        mSymTable.exitScope();
        mCurrentReturnType = savedReturn;
        mSawReturn = savedSawReturn;

        // Build closure function type: fn(ParamTypes) -> ReturnType
        TypeVec paramTypes;
        for (auto& p : le->params) {
            paramTypes.push_back(p.inferredType);
        }
        TypePtr retType = bodyRet;
        le->closureType = Type::makeFunction(paramTypes, retType);

        // Capture analysis: scan body for free variables from enclosing scopes
        // (simplified: just note lambda for codegen, captures resolved later)
        return le->closureType;
    }
    if (auto* as = dynamic_cast<AssignExpr*>(expr)) {
        TypePtr rhs = analyzeExpr(as->rhs.get());
        TypePtr lhs = analyzeExpr(as->lhs.get());
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            if (auto* symbol = mSymTable.lookup(id->name); symbol && symbol->isConst)
                error("cannot assign to const binding '" + id->name + "'", id->line, id->col);
        }
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(index->object.get())) {
                if (auto* symbol = mSymTable.lookup(id->name); symbol && symbol->isConst)
                    error("cannot assign through index of const array binding '" + id->name + "'",
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
                    requireInteger(lhs, "left operand of compound bitwise assignment");
                    requireInteger(rhs, "right operand of compound bitwise assignment");
                    operandsValid =
                        (resolved(lhs)->kind == TypeKind::InferenceVar || isIntegerType(resolved(lhs))) &&
                        (resolved(rhs)->kind == TypeKind::InferenceVar || isIntegerType(resolved(rhs)));
                    break;
                default:
                    requireNumeric(lhs, "left operand of compound arithmetic assignment");
                    requireNumeric(rhs, "right operand of compound arithmetic assignment");
                    break;
            }
        }
        if (operandsValid) constrain(rhs, lhs, "assignment");
        return rhs;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
        TypePtr object = resolved(analyzeExpr(ix->object.get()));
        TypePtr index = resolved(analyzeExpr(ix->index.get()));
        if (!isIntegerType(index))
            error("array index must be an integer, got " + index->toString(), ix->line, ix->col);
        if (object->kind != TypeKind::Array && object->kind != TypeKind::Slice) {
            error("cannot index " + object->toString() + "; only array<T, N> and slice<T> support safe indexing",
                  ix->line, ix->col);
            return TyUnknown;
        }
        if (object->kind == TypeKind::Array) if (auto* literal = dynamic_cast<IntLiteralExpr*>(ix->index.get());
            literal && (literal->value < 0 || static_cast<uint64_t>(literal->value) >= object->arrayLength))
            error("array index " + std::to_string(literal->value) + " is outside array length " +
                  std::to_string(object->arrayLength), ix->line, ix->col);
        return object->inner;
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        if (array->elements.empty()) {
            error("cannot infer the element type of an empty array", array->line, array->col);
            return TyUnknown;
        }
        TypePtr element = analyzeExpr(array->elements.front().get());
        for (size_t i = 1; i < array->elements.size(); ++i)
            constrain(analyzeExpr(array->elements[i].get()), element, "array element");
        array->elementType = resolved(element);
        return Type::makeArray(array->elementType, array->elements.size());
    }
    if (auto* ie = dynamic_cast<IfExpr*>(expr)) {
        requireBool(analyzeExpr(ie->cond.get()), "if-expression condition");
        auto branchType = [this](Expr* branch) {
            if (auto* be = dynamic_cast<BlockExpr*>(branch)) {
                analyzeBlock(be->block.get(), nullptr);
                return TyUnit;
            }
            return analyzeExpr(branch);
        };
        TypePtr thenType = branchType(ie->thenExpr.get());
        TypePtr elseType = branchType(ie->elseExpr.get());
        constrain(thenType, elseType, "if-expression branches");
        return thenType;
    }
    return TyUnknown;
}

TypePtr SemanticAnalyzer::analyzeCall(CallExpr* call) {
    auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get());
    if (id && (id->name == "type_of" || id->name == "type_kind" ||
               id->name == "type_nominal" || id->name == "type_size" ||
               id->name == "type_field_count" || id->name == "type_field_name" ||
               id->name == "type_field_type" || id->name == "type_variant_count" ||
               id->name == "type_variant_name" || id->name == "type_variant_field_count" ||
               id->name == "type_is_struct" || id->name == "type_is_enum" ||
               id->name == "type_is_nominal" || id->name == "type_is_reference"))
        return analyzeReflectionCall(call, id->name);
    if (id && id->name == "slice") {
        if (call->args.size() != 3) { error("slice expects `slice(borrow array, start, end)`", call->line, call->col); return TyUnknown; }
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr source = resolved(analyzeExpr(call->args[0].get()));
        if (!borrow || source->kind != TypeKind::Reference || !source->inner || source->inner->kind != TypeKind::Array) {
            error("slice requires an explicit `borrow array` as its first argument", call->line, call->col); return TyUnknown;
        }
        requireInteger(analyzeExpr(call->args[1].get()), "slice start");
        requireInteger(analyzeExpr(call->args[2].get()), "slice end");
        return Type::makeSlice(source->inner->inner);
    }
    if (id && (id->name == "gpu_alloc_i32" || id->name == "gpu_load_i32" ||
               id->name == "gpu_store_i32" || id->name == "gpu_free" ||
               id->name == "gpu_copy_from_host_i32" ||
               id->name == "gpu_copy_to_host_i32")) {
        auto requireCount = [this, call, id](size_t expected) {
            if (call->args.size() != expected) {
                error("'" + id->name + "' expects " + std::to_string(expected) +
                      " argument" + (expected == 1 ? "" : "s"), call->line, call->col);
                return false;
            }
            return true;
        };
        if (id->name == "gpu_alloc_i32") {
            if (!requireCount(1)) return TyUnknown;
            if (mInKernel)
                error("'gpu_alloc_i32' is a host operation and cannot run inside a kernel", call->line, call->col);
            requireInteger(analyzeExpr(call->args[0].get()), "gpu_alloc_i32 element count");
            return Type::makeDeviceBuffer(TyI32);
        }
        if (id->name == "gpu_free") {
            if (!requireCount(1)) return TyUnknown;
            if (mInKernel)
                error("'gpu_free' is a host operation and cannot run inside a kernel", call->line, call->col);
            if (!dynamic_cast<MoveExpr*>(call->args[0].get()))
                error("'gpu_free' requires `move buffer` to make ownership transfer explicit",
                      call->line, call->col);
            constrain(analyzeExpr(call->args[0].get()), Type::makeDeviceBuffer(TyI32),
                      "gpu_free buffer");
            return TyUnit;
        }
        if (id->name == "gpu_copy_from_host_i32" || id->name == "gpu_copy_to_host_i32") {
            if (!requireCount(3)) return TyUnknown;
            if (mInKernel)
                error("'" + id->name + "' is a host operation and cannot run inside a kernel",
                      call->line, call->col);
            const bool isUpload = id->name == "gpu_copy_from_host_i32";
            auto* deviceBorrow = dynamic_cast<BorrowExpr*>(call->args[0 + (isUpload ? 0 : 1)].get());
            auto* hostBorrow = dynamic_cast<BorrowExpr*>(call->args[1 - (isUpload ? 0 : 1)].get());
            TypePtr deviceActual = analyzeExpr(call->args[isUpload ? 0 : 1].get());
            TypePtr hostActual = analyzeExpr(call->args[isUpload ? 1 : 0].get());
            const bool needsMutableDevice = isUpload;
            const bool needsMutableHost = !isUpload;
            if (!deviceBorrow || deviceBorrow->isMutable != needsMutableDevice)
                error("'" + id->name + "' requires " +
                      std::string(needsMutableDevice ? "`borrow mut buffer`" : "`borrow buffer`") +
                      " for its device-buffer argument", call->line, call->col);
            if (!hostBorrow || hostBorrow->isMutable != needsMutableHost)
                error("'" + id->name + "' requires " +
                      std::string(needsMutableHost ? "`borrow mut host`" : "`borrow host`") +
                      " for its host raw-pointer argument", call->line, call->col);
            constrain(deviceActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), needsMutableDevice),
                      "device-buffer argument of '" + id->name + "'");
            constrain(hostActual,
                      Type::makeReference(Type::makeRawPointer(TyI32), needsMutableHost),
                      "host raw-pointer argument of '" + id->name + "'");
            requireInteger(analyzeExpr(call->args[2].get()),
                           "element count of '" + id->name + "'");
            if (auto* literal = dynamic_cast<IntLiteralExpr*>(call->args[2].get()); literal && literal->value < 0)
                error("'" + id->name + "' requires a non-negative element count",
                      literal->line, literal->col);
            return TyUnit;
        }
        const bool isStore = id->name == "gpu_store_i32";
        if (!requireCount(isStore ? 3 : 2)) return TyUnknown;
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr bufferActual = analyzeExpr(call->args[0].get());
        TypePtr resolvedBufferActual = resolved(bufferActual);
        const bool isDeviceReference = resolvedBufferActual->kind == TypeKind::Reference &&
            resolvedBufferActual->inner &&
            resolvedBufferActual->inner->kind == TypeKind::DeviceBuffer;
        if ((!borrow && !isDeviceReference) || (borrow && isStore && !borrow->isMutable))
            error(std::string("'") + id->name + "' requires an explicit " +
                  (isStore ? "`borrow mut buffer`" : "`borrow buffer`") +
                  " (or an already-borrowed kernel parameter)",
                  call->line, call->col);
        if (isStore) {
            constrain(bufferActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), true),
                      std::string("first argument of '") + id->name + "'");
        } else if (isDeviceReference) {
            // A mutable kernel parameter is also valid for a read-only load.
            constrain(resolvedBufferActual->inner, Type::makeDeviceBuffer(TyI32),
                      std::string("first argument of '") + id->name + "'");
        }
        requireInteger(analyzeExpr(call->args[1].get()), std::string("index argument of '") + id->name + "'");
        if (isStore) constrain(analyzeExpr(call->args[2].get()), TyI32,
                               "value argument of 'gpu_store_i32'");
        return isStore ? TyUnit : TyI32;
    }
    if (mInKernel && id) {
        error("kernel body may only call device built-ins (`gpu_load_i32` and `gpu_store_i32`) in the initial ABI",
              call->line, call->col);
        return TyUnknown;
    }
    SymbolInfo* sym = id ? mSymTable.lookup(id->name) : nullptr;
    auto constrainArgument = [this](Expr* expr, const TypePtr& expected,
                                     const std::string& context) {
        TypePtr actual = analyzeExpr(expr);
        // Integer literals are representationally polymorphic at an FFI
        // boundary; the code generator will widen/truncate them to the ABI
        // parameter width.
        if (dynamic_cast<IntLiteralExpr*>(expr) && isNumericType(resolved(expected)))
            return;
        if (dynamic_cast<StringLiteralExpr*>(expr) &&
            resolved(expected)->kind == TypeKind::CStr)
            return;
        constrain(actual, expected, context);
    };

    if (call->versionSelector) {
        if (!id) {
            error("a version selector can only be applied to a named function");
            return TyUnknown;
        }
        auto* function = selectVersionedFunction(id->name, *call->versionSelector, call);
        if (!function) return TyUnknown;
        if (call->args.size() != function->params.size()) {
            error("Argument count mismatch for versioned function '" + id->name + "'", call->line, call->col);
            return TyUnknown;
        }
        if (!function->typeParams.empty()) {
            TypeVec concreteTypes;
            for (auto& arg : call->args) concreteTypes.push_back(analyzeExpr(arg.get()));
            if (concreteTypes.size() > function->typeParams.size())
                concreteTypes.resize(function->typeParams.size());

            for (auto& clause : function->whereClauses) {
                const std::string& parameterName = clause.typeParam;
                const std::string& traitId = clause.trait.resolvedTraitId;
                for (size_t i = 0; i < function->typeParams.size(); ++i) {
                    if (function->typeParams[i] != parameterName || i >= concreteTypes.size()) continue;
                    const std::string typeName = typeIdentity(resolved(concreteTypes[i]));
                    if (!satisfiesTrait(traitId, resolved(concreteTypes[i])))
                        error("Type '" + typeName + "' does not satisfy trait '" +
                              displayTraitRef(clause.trait) + "'");
                }
            }

            auto* specialized = monomorphize(function, concreteTypes);
            if (!specialized || !mProgram || mGeneratedInstances.empty()) return TyUnknown;
            mProgram->declarations.push_back(std::move(mGeneratedInstances.back()));
            declareFunction(specialized);
            analyzeFunction(specialized);
            call->resolvedSymbolName = specialized->generatedSymbolName.empty()
                ? specialized->name : specialized->generatedSymbolName;
            call->returnsLinear = specialized->returnsLinear;
            return specialized->inferredReturnType ? specialized->inferredReturnType : TyUnit;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), function->params[i].inferredType,
                              "argument " + std::to_string(i + 1) + " of '" + id->name +
                              "@" + call->versionSelector->tag + "'");
        call->resolvedSymbolName = function->generatedSymbolName.empty()
            ? function->name : function->generatedSymbolName;
        call->returnsLinear = function->returnsLinear;
        if (function->isConstexpr) {
            if (auto value = evaluateConstExpr(call)) call->compileTimeValue = std::move(*value);
        }
        return function->inferredReturnType ? function->inferredReturnType : TyUnit;
    }

    if (id && !sym && mVersionedFunctions.count(id->name)) {
        error("function '" + id->name + "' is versioned; select a tag explicitly", call->line, call->col);
        return TyUnknown;
    }

    // Direct calls and closure calls both become constraints. This is the
    // important consumer side of inference: an untyped declaration can be
    // solved by the arguments at its call sites.
    if (!sym) {
        TypePtr calleeType = analyzeExpr(call->callee.get());
        if (calleeType->kind != TypeKind::Function) {
            error("Expression is not callable");
            return TyUnknown;
        }
        if (calleeType->paramTypes.size() != call->args.size()) {
            error("Argument count mismatch in indirect call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), calleeType->paramTypes[i], "call argument");
        return calleeType->returnType;
    }

    if (sym->kind == SymbolKind::Variable && sym->type &&
        resolved(sym->type)->kind == TypeKind::Function) {
        auto fn = resolved(sym->type);
        if (fn->paramTypes.size() != call->args.size()) {
            error("Argument count mismatch in closure call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), fn->paramTypes[i], "closure call argument");
        return fn->returnType;
    }
    if (sym->kind != SymbolKind::Function) {
        error("'" + id->name + "' is not callable");
        return TyUnknown;
    }
    if (call->args.size() != sym->paramTypes.size()) {
        error("Argument count mismatch for '" + id->name + "'");
        return TyUnknown;
    }

    // Generic functions retain their existing monomorphization path. Their
    // type parameters are explicit variables, whereas omitted ordinary
    // signatures use the inference variables in paramTypes/returnType below.
    if (!sym->typeParams.empty() && sym->genericDecl) {
        TypeVec concreteTypes;
        for (auto& arg : call->args) concreteTypes.push_back(analyzeExpr(arg.get()));
        if (concreteTypes.size() > sym->typeParams.size())
            concreteTypes.resize(sym->typeParams.size());

        for (auto& clause : sym->genericDecl->whereClauses) {
            const std::string& tpName = clause.typeParam;
            const std::string& traitId = clause.trait.resolvedTraitId;
            for (size_t j = 0; j < sym->genericDecl->typeParams.size(); ++j) {
                if (sym->genericDecl->typeParams[j] == tpName && j < concreteTypes.size()) {
                    std::string typeName = typeIdentity(resolved(concreteTypes[j]));
                    if (!satisfiesTrait(traitId, resolved(concreteTypes[j])))
                        error("Type '" + typeName + "' does not satisfy trait '" +
                              displayTraitRef(clause.trait) + "'");
                }
            }
        }
        auto* specialized = monomorphize(sym->genericDecl, concreteTypes);
        if (specialized && mProgram) {
            mProgram->declarations.push_back(std::move(mGeneratedInstances.back()));
            declareFunction(specialized);
            analyzeFunction(specialized);
            id->name = specialized->name;
            call->returnsLinear = specialized->returnsLinear;
            return specialized->inferredReturnType
                ? specialized->inferredReturnType : TyUnit;
        }
    }

    // Built-in print is polymorphic but has no user AST to monomorphize.
    if (!sym->typeParams.empty() && !sym->genericDecl) {
        for (auto& arg : call->args) analyzeExpr(arg.get());
        return sym->returnType ? sym->returnType : TyUnit;
    }

    for (size_t i = 0; i < call->args.size(); ++i)
        constrainArgument(call->args[i].get(), sym->paramTypes[i],
                          "argument " + std::to_string(i + 1) + " of '" + id->name + "'");
    if (id) {
        auto constexprIt = mConstexprFunctions.find(id->name);
        if (constexprIt != mConstexprFunctions.end()) {
            if (auto value = evaluateConstExpr(call)) call->compileTimeValue = std::move(*value);
        }
    }
    call->returnsLinear = sym->returnsLinear;
    return sym->returnType ? sym->returnType : TyUnit;
}

TypePtr SemanticAnalyzer::analyzeLaunch(LaunchExpr* launch) {
    if (mInKernel) {
        error("kernel bodies cannot launch another kernel in the initial device ABI",
              launch->line, launch->col);
        return TyUnknown;
    }

    requireInteger(analyzeExpr(launch->threads.get()), "launch thread count");

    FunctionDecl* kernel = nullptr;
    if (launch->kernelSelector) {
        kernel = selectVersionedFunction(launch->kernelName, *launch->kernelSelector, launch);
    } else {
        auto* symbol = mSymTable.lookup(launch->kernelName);
        if (symbol && symbol->kind == SymbolKind::Function)
            kernel = symbol->genericDecl;
        else if (mVersionedFunctions.count(launch->kernelName))
            error("kernel '" + launch->kernelName +
                  "' is versioned; select a tag explicitly in `launch`", launch->line, launch->col);
        else
            error("unknown kernel '" + launch->kernelName + "'", launch->line, launch->col);
    }
    if (!kernel) return TyUnknown;
    if (!kernel->isKernel) {
        error("'" + launch->kernelName + "' is a normal function; only `kernel fn` declarations may be launched",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (kernel->params.empty() || !resolved(kernel->params.front().inferredType)->equals(TyI32)) {
        error("kernel '" + launch->kernelName + "' does not satisfy the required `index: i32` launch ABI",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (launch->args.size() + 1 != kernel->params.size()) {
        error("launch of kernel '" + launch->kernelName + "' expects " +
              std::to_string(kernel->params.size() - 1) + " argument" +
              (kernel->params.size() == 2 ? "" : "s") + " after the implicit index",
              launch->line, launch->col);
        return TyUnknown;
    }

    launch->inFlightResources.clear();
    for (size_t i = 0; i < launch->args.size(); ++i) {
        const TypePtr expected = resolved(kernel->params[i + 1].inferredType);
        const TypePtr actual = analyzeExpr(launch->args[i].get());
        constrain(actual, expected, "launch argument " + std::to_string(i + 1) +
                                   " of kernel '" + launch->kernelName + "'");

        if (expected->kind == TypeKind::Reference && expected->inner &&
            expected->inner->kind == TypeKind::DeviceBuffer) {
            auto* borrow = dynamic_cast<BorrowExpr*>(launch->args[i].get());
            auto* resource = borrow ? dynamic_cast<IdentifierExpr*>(borrow->operand.get()) : nullptr;
            if (!borrow || !resource) {
                error("device-buffer launch argument " + std::to_string(i + 1) +
                      " must be an explicit borrow of a named buffer", launch->line, launch->col);
            } else {
                if (borrow->isMutable != expected->isMutable) {
                    error("device-buffer launch argument " + std::to_string(i + 1) +
                          (expected->isMutable ? " requires `borrow mut`" :
                                                 " requires a shared `borrow`"),
                          launch->line, launch->col);
                }
                launch->inFlightResources.emplace_back(resource->name, expected->isMutable);
            }
        }
    }

    launch->resolvedKernelName = kernel->generatedSymbolName.empty()
        ? kernel->name : kernel->generatedSymbolName;
    return TyEvent;
}

TypePtr SemanticAnalyzer::analyzeReflectionCall(CallExpr* call, const std::string& name) {
    TypePtr type;
    if (!call->typeArgASTs.empty()) {
        if (call->typeArgASTs.size() != 1) {
            error(name + " expects exactly one type argument");
            return TyUnknown;
        }
        type = resolved(resolveTypeAST(call->typeArgASTs[0].get(), {}));
    } else if (call->args.size() == 1) {
        type = resolved(analyzeExpr(call->args[0].get()));
    } else {
        error(name + " expects either `<Type>()` or one value argument",
              call->line, call->col);
        return TyUnknown;
    }

    auto kindName = [](const TypePtr& t) {
        switch (t->kind) {
            case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::USize: case TypeKind::ISize: return std::string("integer");
            case TypeKind::F32: case TypeKind::F64: return std::string("float");
            case TypeKind::Bool: return std::string("bool");
            case TypeKind::String: case TypeKind::CStr: return std::string("string");
            case TypeKind::RawPointer: return std::string("raw_pointer");
            case TypeKind::Reference: return std::string("reference");
            case TypeKind::Function: return std::string("function");
            case TypeKind::Struct: return std::string("struct");
            case TypeKind::Record: return std::string("record");
            case TypeKind::Enum: return std::string("enum");
            case TypeKind::Trait: return std::string("trait");
            case TypeKind::Unit: return std::string("unit");
            default: return std::string("unknown");
        }
    };
    std::function<int64_t(const TypePtr&)> typeSize = [&](const TypePtr& t) -> int64_t {
        switch (t->kind) {
            case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool: return 1;
            case TypeKind::I16: case TypeKind::U16: return 2;
            case TypeKind::I32: case TypeKind::U32: case TypeKind::F32: return 4;
            case TypeKind::I64: case TypeKind::U64: case TypeKind::USize: case TypeKind::ISize:
            case TypeKind::F64: case TypeKind::String: case TypeKind::CStr:
            case TypeKind::RawPointer: case TypeKind::Reference: return 8;
            case TypeKind::Struct: case TypeKind::Record: {
                int64_t total = 0;
                for (const auto& field : t->fields) total += typeSize(field.type);
                return total == 0 ? 8 : total;
            }
            case TypeKind::Enum: return 8;
            default: return 0;
        }
    };
    auto constIndex = [&]() -> std::optional<size_t> {
        if (call->args.size() != 1) {
            error(name + " requires one compile-time integer index");
            return std::nullopt;
        }
        analyzeExpr(call->args[0].get());
        auto value = evaluateConstExpr(call->args[0].get());
        if (!value || !std::holds_alternative<int64_t>(*value) || std::get<int64_t>(*value) < 0) {
            error(name + " requires a non-negative compile-time integer index");
            return std::nullopt;
        }
        return static_cast<size_t>(std::get<int64_t>(*value));
    };

    if (name == "type_of") call->compileTimeValue = type->toString();
    else if (name == "type_kind") call->compileTimeValue = kindName(type);
    else if (name == "type_nominal") call->compileTimeValue = type->nominalId;
    else if (name == "type_size") call->compileTimeValue = typeSize(type);
    else if (name == "type_is_struct") call->compileTimeValue = type->kind == TypeKind::Struct;
    else if (name == "type_is_enum") call->compileTimeValue = type->kind == TypeKind::Enum;
    else if (name == "type_is_nominal") call->compileTimeValue = !type->nominalId.empty();
    else if (name == "type_is_reference") call->compileTimeValue = type->kind == TypeKind::Reference;
    else if (name == "type_field_count") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            error(name + " requires a struct or record type"); return TyUnknown;
        }
        call->compileTimeValue = static_cast<int64_t>(type->fields.size());
    } else if (name == "type_field_name" || name == "type_field_type") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            error(name + " requires a struct or record type"); return TyUnknown;
        }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->fields.size()) {
            error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_field_name"
            ? type->fields[*index].name : type->fields[*index].type->toString();
    } else if (name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) { error(name + " requires an enum type"); return TyUnknown; }
        call->compileTimeValue = static_cast<int64_t>(type->variants.size());
    } else if (name == "type_variant_name" || name == "type_variant_field_count") {
        if (type->kind != TypeKind::Enum) { error(name + " requires an enum type"); return TyUnknown; }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->variants.size()) {
            error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_variant_name"
            ? std::variant<int64_t, double, bool, std::string>(type->variants[*index].name)
            : std::variant<int64_t, double, bool, std::string>(static_cast<int64_t>(type->variants[*index].fields.size()));
    }

    if (name == "type_size" || name == "type_field_count" || name == "type_variant_count" ||
        name == "type_variant_field_count") return TyI32;
    if (name == "type_is_struct" || name == "type_is_enum" || name == "type_is_nominal" ||
        name == "type_is_reference") return TyBool;
    return TyString;
}

void SemanticAnalyzer::enterConstScope() { mConstScopes.emplace_back(); }

void SemanticAnalyzer::exitConstScope() {
    if (mConstScopes.size() > 1) mConstScopes.pop_back();
}

void SemanticAnalyzer::defineConst(const std::string& name, const ConstValue& value) {
    if (mConstScopes.empty()) enterConstScope();
    mConstScopes.back()[name] = value;
}

const SemanticAnalyzer::ConstValue* SemanticAnalyzer::lookupConst(const std::string& name) const {
    for (auto it = mConstScopes.rbegin(); it != mConstScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evaluateConstExpr(Expr* expr, const std::unordered_map<std::string, ConstValue>& locals) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = locals.find(value->name);
        if (local != locals.end()) return local->second;
        if (auto* global = lookupConst(value->name)) return *global;
        return std::nullopt;
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        if (call->compileTimeValue) return *call->compileTimeValue;
        auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        if (!callee) return std::nullopt;
        const std::string& constexprName = call->resolvedSymbolName.empty()
            ? callee->name : call->resolvedSymbolName;
        auto function = mConstexprFunctions.find(constexprName);
        if (function == mConstexprFunctions.end()) return std::nullopt;
        std::vector<ConstValue> args;
        for (auto& arg : call->args) {
            auto value = evaluateConstExpr(arg.get(), locals);
            if (!value) return std::nullopt;
            args.push_back(std::move(*value));
        }
        return evaluateConstFunction(function->second, args);
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateConstExpr(unary->operand.get(), locals);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Minus) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return -*i;
            if (auto* f = std::get_if<double>(&*operand)) return -*f;
        }
        if (unary->op == TokenKind::Not) {
            if (auto* b = std::get_if<bool>(&*operand)) return !*b;
        }
        if (unary->op == TokenKind::Tilde) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return ~*i;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateConstExpr(binary->lhs.get(), locals);
        auto rhs = evaluateConstExpr(binary->rhs.get(), locals);
        if (!lhs || !rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs); auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri; case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri; case TokenKind::Slash: if (*ri) return *li / *ri; break;
                case TokenKind::Percent: if (*ri) return *li % *ri; break;
                case TokenKind::Ampersand: return *li & *ri; case TokenKind::BitOr: return *li | *ri;
                case TokenKind::BitXor: return *li ^ *ri; case TokenKind::ShiftLeft: return *li << *ri;
                case TokenKind::ShiftRight: return *li >> *ri; case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri; case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri; case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri; default: break;
            }
        }
        auto lf = std::get_if<double>(&*lhs); auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
                case TokenKind::Plus: return *lf + *rf; case TokenKind::Minus: return *lf - *rf;
                case TokenKind::Star: return *lf * *rf; case TokenKind::Slash: if (*rf != 0) return *lf / *rf; break;
                case TokenKind::Percent: if (*rf != 0) return std::fmod(*lf, *rf); break;
                case TokenKind::EqEq: return *lf == *rf; case TokenKind::Neq: return *lf != *rf;
                case TokenKind::Lt: return *lf < *rf; case TokenKind::LtEq: return *lf <= *rf;
                case TokenKind::Gt: return *lf > *rf; case TokenKind::GtEq: return *lf >= *rf;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs); auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
    }
    return std::nullopt;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evaluateConstFunction(FunctionDecl* function, const std::vector<ConstValue>& args) {
    if (!function || !function->isConstexpr || function->params.size() != args.size())
        return std::nullopt;
    if (++mConstEvaluationDepth > 128) {
        --mConstEvaluationDepth;
        return std::nullopt;
    }
    std::unordered_map<std::string, ConstValue> locals;
    for (size_t i = 0; i < args.size(); ++i) locals[function->params[i].name] = args[i];
    std::optional<ConstValue> result;
    bool completed = evaluateConstBlock(function->body.get(), locals, result);
    --mConstEvaluationDepth;
    return completed ? result : std::nullopt;
}

bool SemanticAnalyzer::evaluateConstBlock(BlockStmt* block,
                                          std::unordered_map<std::string, ConstValue>& locals,
                                          std::optional<ConstValue>& result) {
    if (!block) return false;
    for (auto& statement : block->stmts) {
        if (auto* let = dynamic_cast<LetStmt*>(statement.get())) {
            auto value = evaluateConstExpr(let->initializer.get(), locals);
            if (!value) return false;
            locals[let->name] = *value;
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(statement.get())) {
            if (!ret->value) return false;
            result = evaluateConstExpr(ret->value.get(), locals);
            return result.has_value();
        } else if (auto* conditional = dynamic_cast<IfStmt*>(statement.get())) {
            auto condition = evaluateConstExpr(conditional->cond.get(), locals);
            auto boolValue = condition ? std::get_if<bool>(&*condition) : nullptr;
            if (!boolValue) return false;
            if (*boolValue) return evaluateConstBlock(conditional->thenBlock.get(), locals, result);
            if (auto* elseBlock = dynamic_cast<BlockStmt*>(conditional->elseBranch.get()))
                return evaluateConstBlock(elseBlock, locals, result);
            return false;
        } else {
            return false;
        }
    }
    return false;
}

FunctionDecl* SemanticAnalyzer::findMatchingImpl(const std::string& traitName,
                                                  const std::string& typeName,
                                                  const std::string& methodName) {
    auto traitIt = mImpls.find(traitName);
    if (traitIt == mImpls.end()) return nullptr;
    auto typeIt = traitIt->second.find(typeName);
    if (typeIt == traitIt->second.end()) return nullptr;
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    return methodIt->second;
}

// ─── AST cloning helpers for monomorphization ────────────────────

static std::unique_ptr<Expr> cloneExpr(
    Expr* src, const std::unordered_map<std::string, TypePtr>& bindings);
static std::unique_ptr<Stmt> cloneStmt(
    Stmt* src, const std::unordered_map<std::string, TypePtr>& bindings);
static std::unique_ptr<BlockStmt> cloneBlock(
    BlockStmt* src, const std::unordered_map<std::string, TypePtr>& bindings);

static std::unique_ptr<Expr> cloneExpr(
    Expr* src, const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!src) return nullptr;
    if (auto* e = dynamic_cast<IntLiteralExpr*>(src))
        return std::make_unique<IntLiteralExpr>(e->value);
    if (auto* e = dynamic_cast<FloatLiteralExpr*>(src))
        return std::make_unique<FloatLiteralExpr>(e->value);
    if (auto* e = dynamic_cast<StringLiteralExpr*>(src))
        return std::make_unique<StringLiteralExpr>(e->value);
    if (auto* e = dynamic_cast<BoolLiteralExpr*>(src))
        return std::make_unique<BoolLiteralExpr>(e->value);
    if (auto* e = dynamic_cast<IdentifierExpr*>(src)) {
        auto it = bindings.find(e->name);
        if (it != bindings.end()) {
            // Substitute type param with concrete literal (0 as placeholder)
            return std::make_unique<IntLiteralExpr>(0);
        }
        return std::make_unique<IdentifierExpr>(e->name);
    }
    if (auto* e = dynamic_cast<BinaryExpr*>(src)) {
        auto c = std::make_unique<BinaryExpr>();
        c->lhs = cloneExpr(e->lhs.get(), bindings);
        c->op = e->op;
        c->rhs = cloneExpr(e->rhs.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<UnaryExpr*>(src)) {
        auto c = std::make_unique<UnaryExpr>();
        c->op = e->op;
        c->operand = cloneExpr(e->operand.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<MoveExpr*>(src)) {
        auto c = std::make_unique<MoveExpr>();
        c->operand = cloneExpr(e->operand.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<BorrowExpr*>(src)) {
        auto c = std::make_unique<BorrowExpr>();
        c->isMutable = e->isMutable;
        c->operand = cloneExpr(e->operand.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<AddrOfExpr*>(src)) {
        auto c = std::make_unique<AddrOfExpr>();
        c->isMutable = e->isMutable;
        c->operand = cloneExpr(e->operand.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<DerefExpr*>(src)) {
        auto c = std::make_unique<DerefExpr>();
        c->operand = cloneExpr(e->operand.get(), bindings);
        return c;
    }
    if (auto* e = dynamic_cast<CallExpr*>(src)) {
        auto c = std::make_unique<CallExpr>();
        c->callee = cloneExpr(e->callee.get(), bindings);
        for (auto& a : e->args) c->args.push_back(cloneExpr(a.get(), bindings));
        return c;
    }
    if (auto* e = dynamic_cast<LaunchExpr*>(src)) {
        auto c = std::make_unique<LaunchExpr>();
        c->kernelName = e->kernelName;
        c->kernelSelector = e->kernelSelector;
        c->resolvedKernelName = e->resolvedKernelName;
        c->threads = cloneExpr(e->threads.get(), bindings);
        for (auto& arg : e->args) c->args.push_back(cloneExpr(arg.get(), bindings));
        return c;
    }
    if (auto* e = dynamic_cast<AssignExpr*>(src)) {
        auto c = std::make_unique<AssignExpr>();
        c->op = e->op;
        c->lhs = cloneExpr(e->lhs.get(), bindings);
        c->rhs = cloneExpr(e->rhs.get(), bindings);
        return c;
    }
    // Default: return placeholder
    return std::make_unique<IntLiteralExpr>(0);
}

static std::unique_ptr<Stmt> cloneStmt(
    Stmt* src, const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!src) return nullptr;
    if (auto* s = dynamic_cast<BlockStmt*>(src))
        return cloneBlock(s, bindings);
    if (auto* s = dynamic_cast<LetStmt*>(src)) {
        auto c = std::make_unique<LetStmt>();
        c->name = s->name;
        c->isLinear = s->isLinear;
        if (s->typeAnnotation) {
            auto* na = dynamic_cast<NamedTypeAST*>(s->typeAnnotation.get());
            std::string typeName = na ? na->name : "i32";
            auto it = bindings.find(typeName);
            c->typeAnnotation = std::make_unique<NamedTypeAST>(
                it != bindings.end() ? it->second->toString() : typeName);
        }
        c->initializer = cloneExpr(s->initializer.get(), bindings);
        return c;
    }
    if (auto* s = dynamic_cast<ReturnStmt*>(src)) {
        auto c = std::make_unique<ReturnStmt>();
        if (s->value) c->value = cloneExpr(s->value.get(), bindings);
        c->autoFrees = s->autoFrees;
        return c;
    }
    if (auto* s = dynamic_cast<ExprStmt*>(src)) {
        auto c = std::make_unique<ExprStmt>();
        c->expr = cloneExpr(s->expr.get(), bindings);
        return c;
    }
    if (auto* s = dynamic_cast<AwaitStmt*>(src)) {
        auto c = std::make_unique<AwaitStmt>();
        c->event = cloneExpr(s->event.get(), bindings);
        return c;
    }
    return std::make_unique<ExprStmt>();
}

static std::unique_ptr<BlockStmt> cloneBlock(
    BlockStmt* src, const std::unordered_map<std::string, TypePtr>& bindings) {
    auto c = std::make_unique<BlockStmt>();
    for (auto& s : src->stmts) {
        c->stmts.push_back(cloneStmt(s.get(), bindings));
    }
    return c;
}

FunctionDecl* SemanticAnalyzer::monomorphize(FunctionDecl* generic, const TypeVec& concreteTypes) {
    auto specialized = std::make_unique<FunctionDecl>();
    specialized->name = generic->name + "_" + std::to_string(mGeneratedInstances.size());
    specialized->isExported = generic->isExported;
    specialized->isKernel = generic->isKernel;
    specialized->isConstexpr = generic->isConstexpr;
    specialized->returnsLinear = generic->returnsLinear;
    specialized->isTemplateInstance = true;
    specialized->concreteTypeArgs = concreteTypes;

    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < generic->typeParams.size() && i < concreteTypes.size(); ++i) {
        bindings[generic->typeParams[i]] = concreteTypes[i];
    }

    for (auto& p : generic->params) {
        Param cp;
        cp.name = p.name;
        cp.isLinear = p.isLinear;
        TypePtr parameterType = p.type
            ? resolveType(p.type.get(), bindings)
            : mConstraints.resolve(p.inferredType);
        if (parameterType && parameterType->kind == TypeKind::TypeParam) {
            auto it = bindings.find(parameterType->name);
            if (it != bindings.end()) parameterType = it->second;
        }
        cp.inferredType = parameterType;
        cp.type = typeToAST(parameterType);
        specialized->params.push_back(std::move(cp));
    }

    if (generic->returnType) {
        specialized->returnType = typeToAST(resolveType(generic->returnType.get(), bindings));
        if (specialized->returnsLinear)
            specialized->returnType = std::make_unique<LinearTypeAST>(
                std::move(specialized->returnType));
        }

    if (generic->body) {
        specialized->body = cloneBlock(generic->body.get(), bindings);
    } else {
        specialized->body = std::make_unique<BlockStmt>();
    }

    mGeneratedInstances.push_back(std::move(specialized));
    return mGeneratedInstances.back().get();
}

TypePtr SemanticAnalyzer::resolveTypeAST(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return TyUnit;
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        // Monomorphization may materialize a version-selected nominal type
        // into an AST annotation. Preserve that exact nominal identity instead
        // of resolving its display name as an untagged type again.
        if (named->resolvedType) return named->resolvedType;
        auto bound = bindings.find(named->name);
        if (bound != bindings.end()) return bound->second;
        if (named->name == "i32") return TyI32;
        if (named->name == "i64") return TyI64;
        if (named->name == "i8") return TyI8;
        if (named->name == "i16") return TyI16;
        if (named->name == "u8") return TyU8;
        if (named->name == "u16") return TyU16;
        if (named->name == "u32") return TyU32;
        if (named->name == "u64") return TyU64;
        if (named->name == "usize") return TyUSize;
        if (named->name == "isize") return TyISize;
        if (named->name == "f32") return TyF32;
        if (named->name == "f64") return TyF64;
        if (named->name == "bool") return TyBool;
        if (named->name == "string") return TyString;
        if (named->name == "cstr") return TyCStr;
        if (named->name == "unit") return TyUnit;
        if (named->name == "Self") return Type::makeTypeParam("Self");

        if (named->name == "raw") {
            if (named->typeArgs.size() != 1) {
                error("raw<T> requires exactly one type argument");
                return TyUnknown;
            }
            return Type::makeRawPointer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "device_buffer") {
            if (named->typeArgs.size() != 1) {
                error("device_buffer<T> requires exactly one element type");
                return TyUnknown;
            }
            return Type::makeDeviceBuffer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "array") {
            if (named->typeArgs.size() != 1 || !named->arrayLength) {
                error("array<T, N> requires one element type and a non-negative compile-time length",
                      named->line, named->col);
                return TyUnknown;
            }
            return Type::makeArray(resolveTypeAST(named->typeArgs[0].get(), bindings),
                                   *named->arrayLength);
        }
        if (named->name == "slice") {
            if (named->typeArgs.size() != 1) { error("slice<T> requires exactly one element type", named->line, named->col); return TyUnknown; }
            return Type::makeSlice(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "event") return TyEvent;

        TypePtr nominalType;
        if (named->versionSelector) {
            nominalType = selectVersionedNominal(named->name, *named->versionSelector, named);
            if (!nominalType || nominalType->kind == TypeKind::Unknown) return TyUnknown;
        } else {
            auto nominal = mNominalTypes.find(named->name);
            if (nominal != mNominalTypes.end()) nominalType = nominal->second;
            else if (mVersionedNominals.count(named->name)) {
                error("type '" + named->name + "' is versioned; select a tag explicitly", named->line, named->col);
                return TyUnknown;
            }
        }
        if (nominalType) {
            TypeVec args;
            for (auto& arg : named->typeArgs)
                args.push_back(resolveTypeAST(arg.get(), bindings));
            const_cast<NamedTypeAST*>(named)->resolvedType = args.empty()
                ? nominalType : instantiateNominal(nominalType, args);
            return const_cast<NamedTypeAST*>(named)->resolvedType;
        }
        return resolveType(ast, bindings);
    }
    if (auto* ref = dynamic_cast<const RefTypeAST*>(ast))
        return Type::makeReference(resolveTypeAST(ref->inner.get(), bindings),
                                   ref->isMutable);
    if (auto* linear = dynamic_cast<const LinearTypeAST*>(ast))
        return resolveTypeAST(linear->inner.get(), bindings);
    if (auto* fn = dynamic_cast<const FunctionTypeAST*>(ast)) {
        TypeVec params;
        for (auto& param : fn->paramTypes)
            params.push_back(resolveTypeAST(param.get(), bindings));
        return Type::makeFunction(std::move(params),
                                  resolveTypeAST(fn->returnType.get(), bindings));
    }
    return TyUnknown;
}

static TypePtr substituteNominalType(
    const TypePtr& type,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!type) return TyUnknown;
    if (type->kind == TypeKind::TypeParam) {
        auto it = bindings.find(type->name);
        return it == bindings.end() ? type : it->second;
    }
    if (type->kind == TypeKind::Reference)
        return Type::makeReference(substituteNominalType(type->inner, bindings),
                                   type->isMutable);
    if (type->kind == TypeKind::RawPointer)
        return Type::makeRawPointer(substituteNominalType(type->inner, bindings));
    if (type->kind == TypeKind::DeviceBuffer)
        return Type::makeDeviceBuffer(substituteNominalType(type->inner, bindings));
    if (type->kind == TypeKind::Function) {
        TypeVec params;
        for (auto& param : type->paramTypes)
            params.push_back(substituteNominalType(param, bindings));
        return Type::makeFunction(std::move(params),
                                  substituteNominalType(type->returnType, bindings));
    }
    if (type->kind == TypeKind::Struct || type->kind == TypeKind::Record) {
        std::vector<TypeField> fields;
        for (auto& field : type->fields)
            fields.push_back({field.name, substituteNominalType(field.type, bindings)});
        auto result = type->kind == TypeKind::Struct
            ? Type::makeStruct(type->name, std::move(fields), type->nominalId)
            : Type::makeRecord(std::move(fields));
        return result;
    }
    if (type->kind == TypeKind::Enum) {
        std::vector<TypeVariant> variants;
        for (auto& variant : type->variants) {
            TypeVariant copied;
            copied.name = variant.name;
            for (auto& field : variant.fields)
                copied.fields.push_back(substituteNominalType(field, bindings));
            variants.push_back(std::move(copied));
        }
        return Type::makeEnum(type->name, std::move(variants), type->nominalId);
    }
    return type;
}

TypePtr SemanticAnalyzer::instantiateNominal(const TypePtr& type,
                                             const std::vector<TypePtr>& args) {
    if (!type || type->typeParams.empty()) return type;
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < type->typeParams.size() && i < args.size(); ++i)
        bindings[type->typeParams[i]] = args[i];
    auto instance = substituteNominalType(type, bindings);
    instance->typeArgs = args;
    instance->typeParams.clear();
    return instance;
}

TypePtr SemanticAnalyzer::declaredType(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return mConstraints.fresh();
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        if (named->name == "auto") return mConstraints.fresh();
    }
    return resolveTypeAST(ast, bindings);
}

TypePtr SemanticAnalyzer::resolved(const TypePtr& type) {
    return mConstraints.resolve(type);
}

bool SemanticAnalyzer::constrain(const TypePtr& actual, const TypePtr& expected,
                                 const std::string& context) {
    if (!actual || !expected || actual->kind == TypeKind::Unknown ||
        expected->kind == TypeKind::Unknown) return true;
    std::string reason;
    if (!mConstraints.unify(actual, expected, &reason)) {
        error("Type constraint failed in " + context + ": " + reason);
        return false;
    }
    return true;
}

void SemanticAnalyzer::requireBool(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireBool(t);
        return;
    }
    if (t->kind != TypeKind::Bool)
        error(context + " must be bool, got " + t->toString());
}

void SemanticAnalyzer::requireNumeric(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireNumeric(t);
        return;
    }
    if (!isNumericType(t))
        error(context + " must be numeric, got " + t->toString());
}

void SemanticAnalyzer::requireInteger(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    // Integer literals and currently-unbound inferred variables default to i32
    // later in inference. Keep them accepted here while rejecting floats and
    // non-numeric values immediately.
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireNumeric(t);
        return;
    }
    if (!isIntegerType(t))
        error(context + " must be an integer, got " + t->toString());
}

void SemanticAnalyzer::checkUnresolved(const TypePtr& type, const std::string& context) {
    if (type && mConstraints.hasUnresolved(type))
        error("Could not infer " + context);
}

std::unique_ptr<TypeAST> SemanticAnalyzer::typeToAST(const TypePtr& type) {
    auto t = resolved(type);
    if (!t || t->kind == TypeKind::Unknown || t->kind == TypeKind::InferenceVar)
        return std::make_unique<NamedTypeAST>("i32");
    if (t->kind == TypeKind::Reference)
        return std::make_unique<RefTypeAST>(typeToAST(t->inner), t->isMutable);
    if (t->kind == TypeKind::RawPointer) {
        auto raw = std::make_unique<NamedTypeAST>("raw");
        raw->typeArgs.push_back(typeToAST(t->inner));
        return raw;
    }
    if (t->kind == TypeKind::DeviceBuffer) {
        auto buffer = std::make_unique<NamedTypeAST>("device_buffer");
        buffer->typeArgs.push_back(typeToAST(t->inner));
        return buffer;
    }
    if (t->kind == TypeKind::Array) {
        auto array = std::make_unique<NamedTypeAST>("array");
        array->typeArgs.push_back(typeToAST(t->inner));
        array->arrayLength = t->arrayLength;
        return array;
    }
    if (t->kind == TypeKind::Slice) {
        auto slice = std::make_unique<NamedTypeAST>("slice");
        slice->typeArgs.push_back(typeToAST(t->inner));
        return slice;
    }
    if (t->kind == TypeKind::Event)
        return std::make_unique<NamedTypeAST>("event");
    if (t->kind == TypeKind::Function) {
        auto fn = std::make_unique<FunctionTypeAST>();
        for (auto& p : t->paramTypes) fn->paramTypes.push_back(typeToAST(p));
        fn->returnType = typeToAST(t->returnType);
        return fn;
    }
    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) {
        auto named = std::make_unique<NamedTypeAST>(t->name);
        named->resolvedType = t;
        for (auto& arg : t->typeArgs)
            named->typeArgs.push_back(typeToAST(arg));
        return named;
    }
    if (t->kind == TypeKind::TypeParam)
        return std::make_unique<NamedTypeAST>(t->name);
    return std::make_unique<NamedTypeAST>(t->toString());
}

void SemanticAnalyzer::materializeInferredTypes(Program* program) {
    std::function<void(Expr*)> visitExpr;
    std::function<void(BlockStmt*)> visitBlock;
    std::function<void(Stmt*)> visitStmt;

    visitExpr = [&](Expr* expr) {
        if (!expr) return;
        if (auto* l = dynamic_cast<LambdaExpr*>(expr)) {
            for (auto& p : l->params) {
                checkUnresolved(p.inferredType, "lambda parameter '" + p.name + "'");
                if (!p.type) p.type = typeToAST(p.inferredType);
            }
            checkUnresolved(l->closureType, "lambda type");
            if (!l->returnType && l->closureType && l->closureType->kind == TypeKind::Function)
                l->returnType = typeToAST(l->closureType->returnType);
            visitBlock(l->body.get());
            return;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(expr)) { visitExpr(b->lhs.get()); visitExpr(b->rhs.get()); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(expr)) { visitExpr(u->operand.get()); return; }
        if (auto* c = dynamic_cast<CallExpr*>(expr)) {
            visitExpr(c->callee.get()); for (auto& a : c->args) visitExpr(a.get()); return;
        }
        if (auto* l = dynamic_cast<LaunchExpr*>(expr)) {
            visitExpr(l->threads.get()); for (auto& a : l->args) visitExpr(a.get()); return;
        }
        if (auto* v = dynamic_cast<VariantConstructExpr*>(expr)) {
            for (auto& a : v->args) visitExpr(a.get()); return;
        }
        if (auto* a = dynamic_cast<AssignExpr*>(expr)) { visitExpr(a->lhs.get()); visitExpr(a->rhs.get()); return; }
        if (auto* f = dynamic_cast<FieldAccessExpr*>(expr)) { visitExpr(f->object.get()); return; }
        if (auto* i = dynamic_cast<IndexExpr*>(expr)) { visitExpr(i->object.get()); visitExpr(i->index.get()); return; }
        if (auto* h = dynamic_cast<HeapAllocExpr*>(expr)) { visitExpr(h->initializer.get()); return; }
        if (auto* m = dynamic_cast<MoveExpr*>(expr)) { visitExpr(m->operand.get()); return; }
        if (auto* b = dynamic_cast<BorrowExpr*>(expr)) { visitExpr(b->operand.get()); return; }
        if (auto* d = dynamic_cast<DerefExpr*>(expr)) { visitExpr(d->operand.get()); return; }
        if (auto* a = dynamic_cast<AddrOfExpr*>(expr)) { visitExpr(a->operand.get()); return; }
        if (auto* i = dynamic_cast<IfExpr*>(expr)) { visitExpr(i->cond.get()); visitExpr(i->thenExpr.get()); visitExpr(i->elseExpr.get()); return; }
        if (auto* b = dynamic_cast<BlockExpr*>(expr)) { visitBlock(b->block.get()); return; }
    };
    visitStmt = [&](Stmt* stmt) {
        if (!stmt) return;
        if (auto* b = dynamic_cast<BlockStmt*>(stmt)) { visitBlock(b); return; }
        if (auto* l = dynamic_cast<LetStmt*>(stmt)) { visitExpr(l->initializer.get()); return; }
        if (auto* r = dynamic_cast<ReturnStmt*>(stmt)) { visitExpr(r->value.get()); return; }
        if (auto* a = dynamic_cast<AwaitStmt*>(stmt)) { visitExpr(a->event.get()); return; }
        if (auto* e = dynamic_cast<ExprStmt*>(stmt)) { visitExpr(e->expr.get()); return; }
        if (auto* i = dynamic_cast<IfStmt*>(stmt)) { visitExpr(i->cond.get()); visitBlock(i->thenBlock.get()); visitStmt(i->elseBranch.get()); return; }
        if (auto* w = dynamic_cast<WhileStmt*>(stmt)) { visitExpr(w->cond.get()); visitBlock(w->body.get()); return; }
        if (auto* f = dynamic_cast<ForStmt*>(stmt)) { visitExpr(f->iterable.get()); visitBlock(f->body.get()); return; }
        if (auto* f = dynamic_cast<FreeStmt*>(stmt)) { visitExpr(f->operand.get()); return; }
    };
    visitBlock = [&](BlockStmt* block) {
        if (!block) return;
        for (auto& stmt : block->stmts) visitStmt(stmt.get());
    };

    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            for (auto& p : f->params) if (!p.type) p.type = typeToAST(p.inferredType);
            if (!f->returnType) f->returnType = typeToAST(f->inferredReturnType);
            visitBlock(f->body.get());
        } else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& f : i->methods) {
                for (auto& p : f->params) if (!p.type) p.type = typeToAST(p.inferredType);
                if (!f->returnType) f->returnType = typeToAST(f->inferredReturnType);
                visitBlock(f->body.get());
            }
        }
    }
}

void SemanticAnalyzer::error(const std::string& msg, int line, int col) {
    if (line <= 0) line = mDiagnosticLine;
    if (col <= 0) col = mDiagnosticCol;
    std::string hint;
    if (msg.find("undefined name") != std::string::npos)
        hint = "declare it with `let`, make sure its spelling matches, or export/import it from its package";
    else if (msg.find("FFI") != std::string::npos || msg.find("ABI") != std::string::npos)
        hint = "C FFI signatures may use only C-compatible scalar, `cstr`, `raw<T>`, or supported reference types";
    else if (msg.find("Argument count mismatch") != std::string::npos)
        hint = "adjust the call arguments to match the function declaration";
    else if (msg.find("has no `@") != std::string::npos ||
             msg.find("has no versioned declarations") != std::string::npos)
        hint = "check the tag and version, or use `name@tag()` to select that tag's latest declaration";
    else if (msg.find("is versioned; select a tag") != std::string::npos)
        hint = "write `name@tag()` for the newest tagged release, or pin `name@tag(x.y.z)`";
    else if (msg.find("not callable") != std::string::npos)
        hint = "call a function value, not an ordinary value";
    else if (msg.find("must be an integer") != std::string::npos)
        hint = "bitwise operators (`&`, `|`, `^`, `~`) and shifts (`<<`, `>>`) only accept integer operands";
    else if (msg.find("const binding") != std::string::npos)
        hint = "use literals, other const bindings, reflection queries, or a `constexpr fn` call with compile-time arguments";
    else if (msg.find("type_") != std::string::npos && msg.find("requires") != std::string::npos)
        hint = "pass a valid type with `<Type>()`; metadata indexes must be non-negative compile-time integers";
    mErrors.push_back(diagnostic::format(
        "semantic", msg, mDiagnosticFile, line, col, hint,
        diagnostic::sourceLineFromFile(mDiagnosticFile, line)));
}

void SemanticAnalyzer::setDiagnosticLocation(const ASTNode* node) {
    if (!node) return;
    if (!node->sourcePath.empty()) mDiagnosticFile = node->sourcePath;
    if (node->line > 0) mDiagnosticLine = node->line;
    if (node->col > 0) mDiagnosticCol = node->col;
}
