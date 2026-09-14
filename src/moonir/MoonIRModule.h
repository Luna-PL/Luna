#pragma once

#include "MoonIRControlFlow.h"

namespace moon {

struct Decl : Node {
    virtual ~Decl() = default;
    std::string packageId;
    std::string declarationId;
    std::string familyId;
    luna::identity::SymbolId symbolId;
    luna::identity::ContractId contractId;
    std::string name;
    std::string generatedSymbolName;
    std::string modulePath;
    bool isExported = false;
    Retention retention = Retention::CompileTime;
    std::vector<MetadataInstance> metadata;
    luna::sysmeta::Facts sysmeta;
};

struct FunctionDecl : Decl {
    bool isKernel = false;
    // Kernel recipes remain in MoonIR for reflection/container export, while
    // only reachable or explicitly reserved kernels reach machine code.
    bool isCodegenReachable = true;
    bool isExtern = false;
    bool isConstexpr = false;
    bool isSelector = false;
    std::string abi;
    std::string linkName;
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    TypeRef returnType;
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    // Construction owns a structured body. Sealing atomically replaces it
    // with the sole executable CFG; the two representations may never coexist.
    std::unique_ptr<BlockStmt> body;
    std::unique_ptr<ControlFlowGraph> controlFlow;
    bool isTemplateInstance = false;
    TypeRefVec concreteTypeArgs;
};

struct FragmentDecl : Decl {
    FragmentKind kind = FragmentKind::Interceptor;
    FragmentCardinality cardinality = FragmentCardinality::Once;
    DeclarationRef targetSlot;
    std::vector<Param> params;
    std::unique_ptr<BlockStmt> body;
    TypeRef structuralType;
};

struct SlotDecl : Decl {
    FragmentKind acceptedKind = FragmentKind::Interceptor;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    std::vector<Param> params;
    DeclarationRef defaultFragment;
    TypeRef structuralType;
};

struct StructDecl : Decl {
    std::vector<std::string> typeParams;
    std::vector<Param> fields;
    TypeRef type;
};

struct EnumDecl : Decl {
    struct Variant {
        std::string name;
        TypeRefVec fields;
    };
    std::vector<std::string> typeParams;
    std::vector<Variant> variants;
    TypeRef type;
};

struct TraitDecl : Decl {
    struct MethodSig {
        std::string name;
        std::vector<Param> params;
        TypeRef returnType;
    };
    std::vector<std::string> typeParams;
    std::vector<Param> traitParams;
    std::vector<MethodSig> methods;
    TypeRef type;
};

struct ImplDecl : Decl {
    std::vector<std::string> typeParams;
    DeclarationRef traitRef;
    TypeRef targetType;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
};

struct FeatureFlags {
    bool runtime = false;
    bool kernel = false;
    bool kernelRuntimeReserved = false;
};

enum class CostKind : uint8_t {
    MachineCode,
    GenericInstantiation,
    RuntimeDescriptor,
    RuntimeMetadata,
    KernelCode,
    ReservedCapability,
};

struct CostItem {
    CostKind kind = CostKind::MachineCode;
    std::string subject;
    std::string reason;
    SourceLocation location;
};

enum class ImportKind : uint8_t { Package, Host };

struct ImportRecord {
    ImportKind kind = ImportKind::Package;
    std::string ownerPackageId;
    std::string localName;
    std::string packageId;
    std::string alias;
    std::string capabilityId;
    std::string linkSymbol;
    std::string abi;
    DeclarationRef declaration;
    TypeRef type;
    SourceLocation location;
};

struct ExportRecord {
    std::string name;
    DeclarationRef declaration;
    TypeRef type;
    DeclarationKind kind = DeclarationKind::Function;
    std::string abi;
    SourceLocation location;
};

struct Module {
    uint32_t formatMajor = FormatMajor;
    uint32_t formatMinor = FormatMinor;
    std::string name;
    struct PackageUse {
        std::string ownerPackageId;
        std::string packageId;
        std::string alias;
    };
    std::vector<std::string> sourceModules;
    std::vector<PackageUse> packageUses;
    bool isPackage = false;
    std::vector<std::string> sourceFiles;
    FeatureFlags features;
    std::vector<TypeRecord> typeTable;
    bool typeTableSealed = false;
    std::vector<MetadataSchema> metadataSchemas;
    std::vector<DeclarationRecord> declarationTable;
    std::vector<ImportRecord> imports;
    std::vector<ExportRecord> exports;
    std::vector<std::unique_ptr<Decl>> declarations;
    std::vector<CostItem> costs;

    std::unordered_map<std::string, Decl*> declarationsById;
    std::unordered_map<std::string, size_t> typesById;
    std::unordered_map<std::string, size_t> declarationRecordsById;
    std::unordered_map<std::string, size_t> declarationRecordsBySymbol;
    std::unordered_map<std::string, size_t> declarationRecordsByLinkage;
    std::unordered_map<std::string, FunctionDecl*> functionsBySymbol;
    std::unordered_map<std::string, FragmentDecl*> fragmentsBySymbol;
    std::unordered_map<std::string, SlotDecl*> slotsBySymbol;

    void rebuildIndexes();
    TypeRef registerType(const TypePtr& type);
    void sealTypeTable();
    const TypeRecord* findType(const TypeRef& id) const;
    const DeclarationRecord* findDeclaration(
        const SymbolRef& symbol) const;
    const DeclarationRecord* findDeclaration(
        const DeclarationRef& reference) const;
    const DeclarationRecord* findDeclarationById(
        const std::string& id) const;
    const DeclarationRecord* findDeclarationByLinkage(
        const std::string& linkage) const;
};

// Backends may materialize their preferred Type graph from canonical records.
// This cache is deliberately external to Module: it is neither serialized nor
// consulted by the verifier and contains no frontend-owned Type object.
class TypeMaterializer {
public:
    explicit TypeMaterializer(const Module& module) : mModule(module) {}
    TypePtr materialize(const TypeRef& reference);

private:
    const Module& mModule;
    std::unordered_map<std::string, TypePtr> mCache;
};

const char* retentionName(Retention retention);
const char* declarationKindName(DeclarationKind kind);
const char* costKindName(CostKind kind);
const char* operatorName(Operator op);
const char* regionKindName(RegionKind kind);
const char* terminatorKindName(TerminatorKind kind);
const char* typeDomainName(luna::types::TypeDomain domain);
const char* identityModeName(luna::types::IdentityMode mode);

// The complete set of compiler intrinsic names that have no MoonIR
// declaration table row. Shared by the ControlFlowBuilder and Verifier so
// the canonical path does not reject intrinsic callees as unresolved
// identifiers. Keeping this in one place prevents drift.
bool isCompilerIntrinsicName(const std::string& name);

} // namespace moon
