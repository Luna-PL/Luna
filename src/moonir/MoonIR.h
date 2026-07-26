#pragma once

#include "../core/TypeSystem.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace moon {

inline constexpr uint32_t FormatMajor = 0;
inline constexpr uint32_t FormatMinor = 2;

struct SourceLocation {
    std::string path;
    int line = 0;
    int column = 0;
};

struct Node {
    virtual ~Node() = default;
    SourceLocation location;
};

enum class Retention : uint8_t {
    CompileTime,
    Runtime,
    Dynamic,
};

enum class DeclarationKind : uint8_t {
    Function,
    Fragment,
    Struct,
    Enum,
    Trait,
    Implementation,
    MetadataSchema,
};

enum class FragmentKind : uint8_t { Interceptor, Context };
enum class FragmentCardinality : uint8_t { Once, Many };

// MoonIR owns its operator vocabulary. LLVM lowering must not depend on lexer
// token values or source spelling.
enum class Operator : uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Assign,
    AddAssign,
    SubtractAssign,
    MultiplyAssign,
    DivideAssign,
    RemainderAssign,
    BitAndAssign,
    BitOrAssign,
    BitXorAssign,
    ShiftLeftAssign,
    ShiftRightAssign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    ShiftLeft,
    ShiftRight,
    LogicalAnd,
    LogicalOr,
    BitAnd,
    BitOr,
    BitXor,
    LogicalNot,
    BitNot,
    Dereference,
    Negate,
};

using ConstantValue = std::variant<int64_t, double, bool, std::string>;

struct MetadataField {
    std::string name;
    TypePtr type;
};

struct MetadataSchema {
    std::string id;
    std::string name;
    std::vector<MetadataField> fields;
    SourceLocation location;
};

struct MetadataInstance {
    std::string schemaId;
    std::vector<ConstantValue> values;
    Retention retention = Retention::CompileTime;
    SourceLocation location;
};

// Serialized MoonIR refers to immutable type-table entries rather than
// frontend-owned Type pointers. TypePtr remains on operation nodes during the
// migration, but every reachable resolved type must have one verified entry.
struct TypeRecord {
    luna::types::TypeId id;
    luna::types::ShapeId shapeId;
    luna::types::TypeDomain domain = luna::types::TypeDomain::Value;
    luna::types::IdentityMode identityMode = luna::types::IdentityMode::Structural;
    TypeKind kind = TypeKind::Unknown;
    luna::sysmeta::Facts sysmeta;
    std::string displayName;
    std::string nominalDeclarationId;
    std::string canonicalType;
    std::string canonicalShape;
    // Immediate graph edges make the frozen table independently traversable;
    // Moon readers never need a frontend-owned Type pointer to discover the
    // complete closed type graph.
    std::vector<luna::types::TypeId> referencedTypeIds;
};

struct DeclarationRecord {
    std::string id;
    std::string familyId;
    std::string sourceName;
    std::string linkageName;
    DeclarationKind kind = DeclarationKind::Function;
    Retention retention = Retention::CompileTime;
    std::vector<MetadataInstance> metadata;
    TypePtr type;
    luna::sysmeta::Facts sysmeta;
    SourceLocation location;
};

struct Param {
    std::string name;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
    TypePtr type;
};

struct CleanupObligation {
    std::string place;
    luna::ownership::CleanupAction action = luna::ownership::CleanupAction::Drop;
    luna::types::TypeId typeId;
};

struct Expr;
struct Stmt;
struct BlockStmt;

struct Stmt : Node {};

struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

struct LetStmt : Stmt {
    std::string name;
    bool isConst = false;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    TypePtr type;
    std::unique_ptr<Expr> initializer;
};

struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;
    std::vector<std::string> autoFrees;
    std::vector<CleanupObligation> cleanups;
};

struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> thenBlock;
    std::unique_ptr<Stmt> elseBranch;
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> body;
};

struct ForStmt : Stmt {
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<BlockStmt> body;
};

struct FreeStmt : Stmt {
    std::unique_ptr<Expr> operand;
    luna::ownership::CleanupAction action =
        luna::ownership::CleanupAction::Deallocate;
};

struct SlotDeclStmt : Stmt {
    std::string name;
    FragmentKind acceptedKind = FragmentKind::Interceptor;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    bool isDynamic = false;
    std::vector<Param> params;
    std::string defaultFragment;
    std::string resolvedDefaultFragmentName;
    TypePtr structuralType;
};

struct SlotInvokeStmt : Stmt {
    std::string name;
    FragmentKind acceptedKind = FragmentKind::Interceptor;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    bool isDynamic = false;
    bool usesDynamicDispatch = false;
    std::vector<std::string> resolvedDynamicFragmentNames;
    std::vector<std::unique_ptr<Expr>> args;
    std::unique_ptr<BlockStmt> continuation;
    bool isImplicitCapture = false;
    std::vector<Param> interfaceParams;
    std::vector<std::string> resolvedParamNames;
    std::string defaultFragment;
    std::string resolvedDefaultFragmentName;
    TypePtr structuralType;
};

struct ResumeStmt : Stmt {};
struct AbortStmt : Stmt {
    std::vector<std::string> autoFrees;
    std::vector<CleanupObligation> cleanups;
};

struct AwaitStmt : Stmt {
    std::unique_ptr<Expr> event;
};

struct ApplyStmt : Stmt {
    std::string slotName;
    std::string fragmentName;
    bool isDynamic = false;
    std::vector<std::string> alternativeFragmentNames;
    std::vector<std::string> resolvedAlternativeFragmentNames;
    std::string resolvedFragmentName;
    std::unique_ptr<BlockStmt> body;
};

struct Expr : Node {
    TypePtr type;
};

struct IntLiteralExpr : Expr {
    int64_t value = 0;
};

struct FloatLiteralExpr : Expr {
    double value = 0.0;
};

struct StringLiteralExpr : Expr {
    std::string value;
};

struct BoolLiteralExpr : Expr {
    bool value = false;
};

struct IdentifierExpr : Expr {
    std::string name;
};

struct BinaryExpr : Expr {
    std::unique_ptr<Expr> lhs;
    Operator op = Operator::Add;
    std::unique_ptr<Expr> rhs;
};

struct UnaryExpr : Expr {
    Operator op = Operator::Negate;
    std::unique_ptr<Expr> operand;
};

struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    TypeVec typeArgs;
    std::string resolvedSymbolName;
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    TypePtr intrinsicType;
    std::optional<ConstantValue> compileTimeValue;
};

struct DynamicSelectCandidate {
    std::string declarationId;
    std::string linkageName;
    std::vector<ConstantValue> metadataValues;
};

// A dynamic select is an explicit host-side binding operation.  Its finite
// candidate set and runtime-visible metadata are embedded in MoonIR so a
// backend or future MoonRuntime can validate the operation before execution.
struct DynamicSelectExpr : Expr {
    std::string familyId;
    std::string selectorDeclarationId;
    std::string metadataSchemaId;
    std::vector<std::unique_ptr<Expr>> filterArguments;
    std::vector<DynamicSelectCandidate> candidates;
};

struct LaunchExpr : Expr {
    std::string kernelName;
    std::string resolvedKernelName;
    std::unique_ptr<Expr> threads;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::pair<std::string, bool>> inFlightResources;
};

struct VariantConstructExpr : Expr {
    std::string typeName;
    std::string variantName;
    std::vector<std::unique_ptr<Expr>> args;
    TypePtr constructedType;
};

struct FieldAccessExpr : Expr {
    std::unique_ptr<Expr> object;
    std::string field;
};

struct IndexExpr : Expr {
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};

struct ArrayLiteralExpr : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
    TypePtr elementType;
};

struct HeapAllocExpr : Expr {
    std::unique_ptr<Expr> initializer;
    TypePtr allocatedType;
    HeapStorageKind storage = HeapStorageKind::Unique;
};

struct TryExpr : Expr {
    std::unique_ptr<Expr> operand;
    TypePtr resultType;
    TypePtr valueType;
    TypePtr errorType;
    std::vector<CleanupObligation> cleanups;
};

struct MoveExpr : Expr {
    std::unique_ptr<Expr> operand;
};

struct BorrowExpr : Expr {
    bool isMutable = false;
    std::unique_ptr<Expr> operand;
};

struct DerefExpr : Expr {
    std::unique_ptr<Expr> operand;
};

struct AddrOfExpr : Expr {
    bool isMutable = false;
    std::unique_ptr<Expr> operand;
};

struct BlockExpr : Expr {
    std::unique_ptr<BlockStmt> block;
};

struct IfExpr : Expr {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
};

struct LambdaExpr : Expr {
    std::vector<Param> params;
    TypePtr returnType;
    std::unique_ptr<BlockStmt> body;
    TypePtr closureType;
    std::vector<std::string> captures;
    std::string identitySuffix;
};

struct AssignExpr : Expr {
    Operator op = Operator::Assign;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

struct Decl : Node {
    virtual ~Decl() = default;
    std::string packageId;
    std::string declarationId;
    std::string familyId;
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
    TypePtr returnType;
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    std::unique_ptr<BlockStmt> body;
    bool isTemplateInstance = false;
    TypeVec concreteTypeArgs;
};

struct FragmentDecl : Decl {
    FragmentKind kind = FragmentKind::Interceptor;
    FragmentCardinality cardinality = FragmentCardinality::Once;
    std::vector<Param> params;
    std::unique_ptr<BlockStmt> body;
    TypePtr structuralType;
};

struct StructDecl : Decl {
    std::vector<std::string> typeParams;
    std::vector<Param> fields;
    TypePtr type;
};

struct EnumDecl : Decl {
    struct Variant {
        std::string name;
        TypeVec fields;
    };
    std::vector<std::string> typeParams;
    std::vector<Variant> variants;
    TypePtr type;
};

struct TraitDecl : Decl {
    struct MethodSig {
        std::string name;
        std::vector<Param> params;
        TypePtr returnType;
    };
    std::vector<std::string> typeParams;
    std::vector<Param> traitParams;
    std::vector<MethodSig> methods;
    TypePtr type;
};

struct ImplDecl : Decl {
    std::vector<std::string> typeParams;
    std::string resolvedTraitId;
    std::string resolvedTargetTypeId;
    TypePtr targetType;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
};

struct FeatureFlags {
    bool runtime = false;
    bool dynamicReflection = false;
    bool dynamicApply = false;
    bool dynamicSelect = false;
    bool kernel = false;
    bool kernelRuntimeReserved = false;
};

enum class CostKind : uint8_t {
    MachineCode,
    GenericInstantiation,
    RuntimeDescriptor,
    RuntimeMetadata,
    DynamicBinding,
    KernelCode,
    ReservedCapability,
};

struct CostItem {
    CostKind kind = CostKind::MachineCode;
    std::string subject;
    std::string reason;
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
    std::vector<std::unique_ptr<Decl>> declarations;
    std::vector<CostItem> costs;

    std::unordered_map<std::string, Decl*> declarationsById;
    std::unordered_map<std::string, size_t> typesById;
    std::unordered_map<std::string, FunctionDecl*> functionsBySymbol;
    std::unordered_map<std::string, FragmentDecl*> fragmentsBySymbol;

    void rebuildIndexes();
    void registerType(const TypePtr& type);
    void sealTypeTable();
    const TypeRecord* findType(const luna::types::TypeId& id) const;
};

const char* retentionName(Retention retention);
const char* declarationKindName(DeclarationKind kind);
const char* costKindName(CostKind kind);
const char* operatorName(Operator op);
const char* typeDomainName(luna::types::TypeDomain domain);
const char* identityModeName(luna::types::IdentityMode mode);

} // namespace moon
