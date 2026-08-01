#pragma once

#include "../lexer/Token.h"
#include "../core/Ownership.h"
#include "../core/TypeSystem.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <cstdint>

// ─── Forward declarations ────────────────────────────────────────────
struct Param;
struct Expr;
struct Stmt;
struct BlockStmt;
struct TypeAST;

enum class FragmentKind { Interceptor, Context };
enum class FragmentCardinality { Once, Many };
enum class RetentionKind { CompileTime, Runtime, Dynamic };
using MetadataConstValue = std::variant<int64_t, double, bool, std::string>;

// ─── AST Node base ───────────────────────────────────────────────────
struct ASTNode {
    virtual ~ASTNode() = default;
    std::string sourcePath;
    int line = 0;
    int col = 0;
};

struct TraitRef : ASTNode {
    std::string name;
    std::vector<std::unique_ptr<TypeAST>> typeArgs;
    TypeVec resolvedTypeArgs;
    std::string resolvedTraitId;
};

struct WhereClause {
    enum class Kind { TraitBound, Constraint };
    Kind kind = Kind::TraitBound;
    std::string typeParam;
    TraitRef trait;
    std::string constraintName;
    std::vector<std::unique_ptr<TypeAST>> constraintTypeArgs;

    WhereClause() = default;
    WhereClause(WhereClause&&) noexcept = default;
    WhereClause& operator=(WhereClause&&) noexcept = default;
    WhereClause(const WhereClause&) = delete;
    WhereClause& operator=(const WhereClause&) = delete;
};

// ─── Types (as AST nodes, used during parsing) ───────────────────────
struct TypeAST : ASTNode {};
struct NamedTypeAST : TypeAST {
    std::string name;
    std::vector<std::unique_ptr<TypeAST>> typeArgs;
    // `array<T, N>` keeps its extent in the type AST.  It is deliberately a
    // compile-time integer rather than an expression: array layout is part of
    // the ABI and must never depend on a runtime value.
    std::optional<uint64_t> arrayLength;
    TypePtr resolvedType;
    explicit NamedTypeAST(std::string n) : name(std::move(n)) {}
};
struct RefTypeAST : TypeAST {
    std::unique_ptr<TypeAST> inner;
    bool isMutable = false;
    explicit RefTypeAST(std::unique_ptr<TypeAST> i, bool mutableRef = false)
        : inner(std::move(i)), isMutable(mutableRef) {}
};
struct LinearTypeAST : TypeAST {
    std::unique_ptr<TypeAST> inner;
    explicit LinearTypeAST(std::unique_ptr<TypeAST> i) : inner(std::move(i)) {}
};
struct AffineTypeAST : TypeAST {
    std::unique_ptr<TypeAST> inner;
    explicit AffineTypeAST(std::unique_ptr<TypeAST> i) : inner(std::move(i)) {}
};
struct FunctionTypeAST : TypeAST {
    std::vector<std::unique_ptr<TypeAST>> paramTypes;
    std::unique_ptr<TypeAST> returnType;
};

// ─── Statements ──────────────────────────────────────────────────────
struct Stmt : ASTNode {};

struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

struct LetStmt : Stmt {
    std::string name;
    bool isConst = false;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    bool hasExplicitUsage = false;
    std::unique_ptr<TypeAST> typeAnnotation; // optional
    std::unique_ptr<Expr> initializer;
    // The typed frontend records every binding's canonical type for MoonIR
    // lowering. Backend passes never consult the scoped SymbolTable.
    TypePtr inferredType;
    // A compiler iterator recipe may be materialized as a single-consumption
    // stack value. An owning recipe records its hidden source witness so the
    // ownership and backend passes can close every still-initialized element.
    bool materializesIteratorRecipe = false;
    bool materializedIteratorOwnsSource = false;
    TypePtr materializedIteratorSourceType;
};

// A path-specific cleanup is attached to the control transfer that leaves
// the owning scope.  Keeping this independent from ReturnStmt is important:
// fragment abort and future control-exit edges need the same resource
// semantics.
struct CleanupObligation {
    std::string place;
    luna::ownership::CleanupAction action = luna::ownership::CleanupAction::Drop;
    TypePtr type;
};

struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value; // nullptr for unit return
    // Filled by ownership checking.  These are non-linear heap values that
    // remain live on this exact return path and therefore need runtime
    // cleanup before the LLVM `ret` is emitted.
    std::vector<std::string> autoFrees;
    std::vector<CleanupObligation> cleanups;
};

struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> thenBlock;
    std::unique_ptr<Stmt> elseBranch; // BlockStmt or IfStmt
};

struct MatchArm : ASTNode {
    std::string typeQualifier;
    std::string variantName;
    std::vector<std::string> bindings;
    size_t variantIndex = 0;
    TypeVec bindingTypes;
    std::unique_ptr<BlockStmt> body;
};

struct MatchStmt : Stmt {
    std::unique_ptr<Expr> scrutinee;
    TypePtr matchedType;
    std::vector<MatchArm> arms;
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> body;
};

struct ForStmt : Stmt {
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<BlockStmt> body;
    TypePtr elementType;
    // Empty keeps the compiler-fused array/slice/range recipe path.  A
    // non-empty symbol denotes the exact Core Iterator::next implementation
    // selected by semantic analysis; codegen must not repeat trait lookup.
    std::string protocolNextSymbol;
    TypePtr protocolIteratorType;
    TypePtr protocolOptionType;
    size_t protocolNoneVariant = 0;
    size_t protocolSomeVariant = 0;
    // When non-empty, `for source` consumes source through the unique Core
    // IntoIterator impl exactly once and owns the resulting hidden iterator
    // state until None or an enclosing early return.
    std::string protocolIntoSymbol;
    TypePtr protocolInputType;
    std::string protocolStateName;
    bool protocolStateNeedsCleanup = false;
    luna::ownership::CleanupAction protocolStateCleanup =
        luna::ownership::CleanupAction::Deallocate;
    // A move-only compiler recipe owns a stack snapshot of its source array.
    // Per-element initialization bits make partial consumption recoverable.
    std::string recipeStateName;
    TypePtr recipeSourceType;
};

struct FreeStmt : Stmt {
    std::unique_ptr<Expr> operand;
    luna::ownership::CleanupAction action =
        luna::ownership::CleanupAction::Deallocate;
};

// A local slot either declares an explicit interface (`slot s(x: T);`) or
// creates an implicit-capture continuation at its current source position
// (`slot s { ... }`). The semantic pass fills its structural type and effects.
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

// `name(args) { ... }` is only valid for a previously declared explicit slot.
// The block is the continuation run by a fragment's `resume()`.
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
    // Fragment-local affine owners that must be released before control skips
    // the continuation and rejoins after the slot.
    std::vector<std::string> autoFrees;
    std::vector<CleanupObligation> cleanups;
};

struct AwaitStmt : Stmt {
    std::unique_ptr<Expr> event;
};

// Bind a named fragment to a slot for either an explicit block or the
// remainder of the current lexical scope.
struct ApplyStmt : Stmt {
    std::string slotName;
    std::string fragmentName;
    bool isDynamic = false;
    // A dynamic apply declares a finite, type-checked candidate set. The
    // first entry remains the deterministic fallback when no runtime choice
    // is installed for this slot.
    std::vector<std::string> alternativeFragmentNames;
    std::vector<std::string> resolvedAlternativeFragmentNames;
    std::string resolvedFragmentName;
    std::unique_ptr<BlockStmt> body;
};

// ─── Expressions ─────────────────────────────────────────────────────
struct Expr : ASTNode {};

struct IntLiteralExpr : Expr {
    int64_t value;
    explicit IntLiteralExpr(int64_t v) : value(v) {}
};

struct FloatLiteralExpr : Expr {
    double value;
    explicit FloatLiteralExpr(double v) : value(v) {}
};

struct StringLiteralExpr : Expr {
    std::string value;
    explicit StringLiteralExpr(std::string v) : value(std::move(v)) {}
};

struct BoolLiteralExpr : Expr {
    bool value;
    explicit BoolLiteralExpr(bool v) : value(v) {}
};

struct IdentifierExpr : Expr {
    std::string name;
    explicit IdentifierExpr(std::string n) : name(std::move(n)) {}
};

struct BinaryExpr : Expr {
    std::unique_ptr<Expr> lhs;
    TokenKind op;
    std::unique_ptr<Expr> rhs;
};

struct UnaryExpr : Expr {
    TokenKind op;
    std::unique_ptr<Expr> operand;
};

struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    TypeVec typeArgs;
    std::vector<std::unique_ptr<TypeAST>> typeArgASTs;
    // Semantic analysis writes the hygienic symbol selected by metadata or
    // generic instantiation.
    std::string resolvedSymbolName;
    // `linear` is an ownership qualifier rather than an LLVM ABI type. Keep
    // it on calls so the ownership pass can enforce an owning FFI result even
    // though the resolved value type is still `raw<T>`.
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    TypePtr intrinsicType;
    TypePtr resultType;
    TypePtr iteratorInputType;
    TypePtr iteratorOutputType;
    IteratorOp iteratorOp = IteratorOp::None;
    // A move-only terminal recipe consumes a local array source and owns a
    // hidden snapshot until the terminal has produced its result.
    std::string iteratorRecipeStateName;
    TypePtr iteratorRecipeSourceType;
    // `collect::<Target>()` consumes a compiler-only iterator recipe through
    // one exact Core FromIterator builder implementation. These witnesses
    // keep static protocol selection explicit across MoonIR lowering.
    TypePtr iteratorCollectTargetType;
    TypePtr iteratorCollectBuilderType;
    std::string iteratorCollectBeginSymbol;
    std::string iteratorCollectPushSymbol;
    std::string iteratorCollectFinishSymbol;
    std::optional<std::variant<int64_t, double, bool, std::string>> compileTimeValue;
    // Static declaration reflection keeps identity in the frontend. It is
    // erased after a surrounding reflection query or select is folded.
    std::string compileTimeDeclarationId;
};

// A launch is an expression so the returned event can be held in a linear
// binding and awaited explicitly. `threads` is the one-dimensional dispatch
// size used by the CPU simulator and by the initial GPU ABI.
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
    std::vector<std::unique_ptr<TypeAST>> typeArgs;
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
    std::unique_ptr<Expr> initializer; // CallExpr to construct
    std::unique_ptr<TypeAST> allocatedTypeAST;
    TypePtr allocatedType;             // Set during sema
    TypePtr resultType;                // Unique T, rc<T>, or arc<T>
    HeapStorageKind storage = HeapStorageKind::Unique;
};

struct TryExpr : Expr {
    std::unique_ptr<Expr> operand;
    TypePtr resultType;
    TypePtr propagatedResultType;
    TypePtr valueType;
    TypePtr errorType;
    TypePtr propagatedErrorType;
    std::string errorConversionSymbol;
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
    explicit BlockExpr(std::unique_ptr<BlockStmt> b) : block(std::move(b)) {}
};

struct IfExpr : Expr {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
};

struct LambdaExpr : Expr {
    std::vector<Param> params;
    std::unique_ptr<TypeAST> returnType; // nullptr = auto-infer
    std::unique_ptr<BlockStmt> body;
    // Set during sema (closure analysis)
    TypePtr closureType;
    std::vector<std::string> captures;
};

struct AssignExpr : Expr {
    TokenKind op = TokenKind::Eq;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

// A selector is an ordinary function over a real built-in DeclarationView.
// The declaration writes that first parameter explicitly; the select
// expression supplies the finite view according to the public protocol and
// resolves the returned DeclarationRef before static MoonIR lowering.
struct SelectExpr : Expr {
    struct DynamicFilterArgument {
        // Index into selectorArgs when the selector protocol forwards one of
        // its explicit parameters; otherwise the binding is a literal.
        std::optional<size_t> selectorArgumentIndex;
        std::optional<MetadataConstValue> constant;
    };
    struct DynamicCandidate {
        std::string declarationId;
        std::string symbolName;
        std::vector<MetadataConstValue> metadataValues;
    };
    std::string targetName;
    std::string selectorName;
    std::vector<std::unique_ptr<Expr>> selectorArgs;
    bool isDynamic = false;
    std::string resolvedDeclarationId;
    std::string resolvedSymbolName;
    std::string resolvedFamilyId;
    std::string resolvedSelectorDeclarationId;
    std::vector<std::string> dynamicCandidateIds;
    std::string dynamicMetadataSchemaId;
    std::vector<DynamicFilterArgument> dynamicFilterArguments;
    std::vector<DynamicCandidate> dynamicCandidates;
    TypePtr selectedType;
};

// ─── Patterns ─────────────────────────────────────────────────────────
struct Pattern : ASTNode {};
struct IdentPattern : Pattern {
    std::string name;
    TypePtr type;
    explicit IdentPattern(std::string n) : name(std::move(n)) {}
};

// ─── Declarations ────────────────────────────────────────────────────
struct Decl : ASTNode {
    // The declaration location starts at its first modifier/keyword. Tooling
    // keeps the identifier selection separately so navigation lands on the
    // source name even for exported or metadata-attached declarations.
    int nameLine = 0;
    int nameCol = 0;
    // Every declaration keeps its owning Package ID independently from its
    // module path. The package loader fills this field before semantic
    // analysis, including for declarations loaded from dependencies.
    std::string packageId;
    // Empty denotes the package root module. Module identity is source-level
    // namespace information and remains separate from the Package ID.
    std::string modulePath;
    // Package declarations are private by default. An explicit export is the
    // only way a declaration becomes part of the package's public interface.
    bool isExported = false;
    // Metadata-distinguished declarations share a source family but always
    // receive a unique generated linkage identity.
    std::string generatedSymbolName;
    RetentionKind retention = RetentionKind::CompileTime;
    struct MetadataAttachment {
        std::string schemaName;
        std::vector<std::unique_ptr<Expr>> arguments;
        RetentionKind retention = RetentionKind::CompileTime;
        std::string resolvedSchemaId;
        std::vector<MetadataConstValue> evaluatedArguments;
    };
    std::vector<MetadataAttachment> metadata;
};

struct Param {
    std::string name;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    bool hasExplicitUsage = false;
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
    std::unique_ptr<TypeAST> type;
    TypePtr inferredType; // Filled by constraint-based semantic analysis.
};

struct FunctionDecl : Decl {
    std::string name;
    bool isKernel = false;
    bool isExtern = false;
    bool isConstexpr = false;
    bool isSelector = false;
    bool isDynamicSelector = false;
    std::string abi;
    std::string linkName;
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    std::unique_ptr<TypeAST> returnType;
    // Preserves a direct `-> linear raw<T>` return annotation. Type lowering
    // intentionally erases `linear`, so ownership needs this separate bit.
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    TypePtr inferredReturnType; // Filled when return type is omitted/auto.
    std::vector<WhereClause> whereClauses;
    std::unique_ptr<BlockStmt> body;
    bool isTemplateInstance = false;
    TypeVec concreteTypeArgs;
};

struct FragmentDecl : Decl {
    std::string name;
    FragmentKind kind = FragmentKind::Interceptor;
    FragmentCardinality cardinality = FragmentCardinality::Once;
    // Explicit fragment parameters form the reusable-slot input interface.
    // Empty means the fragment has no declared inputs and may bind an
    // implicit-capture slot.
    std::vector<Param> params;
    std::unique_ptr<BlockStmt> body;
    TypePtr structuralType;
};

struct StructDecl : Decl {
    std::string name;
    bool isNominal = false;
    std::vector<std::string> typeParams;
    std::vector<Param> fields;
};

// Algebraic data type sum declaration. StructDecl is the product case;
// EnumDecl is the sum case. Variants currently use tuple payloads, leaving
// room for record-style variants without changing the nominal binding model.
struct EnumDecl : Decl {
    struct Variant {
        std::string name;
        std::vector<std::unique_ptr<TypeAST>> fields;
    };
    std::string name;
    bool isNominal = false;
    std::vector<std::string> typeParams;
    std::vector<Variant> variants;
};

struct TraitDecl : Decl {
    std::string name;
    std::string resolvedTraitId;
    std::vector<std::string> typeParams;
    std::vector<Param> traitParams;
    struct MethodSig {
        std::string name;
        std::vector<Param> params;
        std::unique_ptr<TypeAST> returnType;
        TypePtr inferredReturnType;
    };
    std::vector<MethodSig> methods;
};

struct ImplDecl : Decl {
    std::vector<std::string> typeParams;
    TraitRef trait;
    std::unique_ptr<TypeAST> targetType;
    std::string resolvedTargetTypeId;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
};

struct MetaDecl : Decl {
    struct Field {
        std::string name;
        std::unique_ptr<TypeAST> type;
        TypePtr inferredType;
    };
    std::string name;
    std::vector<Field> fields;
};

// A named compile-time boolean predicate over types. Constraints have no
// runtime representation; they are evaluated at generic instantiation sites.
struct ConstraintDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::unique_ptr<Expr> predicate;
};

struct Program : ASTNode {
    struct PackageUse : ASTNode {
        // Package whose source declared this alias. Aliases are package-local
        // and therefore may be reused by unrelated dependency packages.
        std::string ownerPackageId;
        std::string packageId;
        std::string alias;
    };
    // Canonical package identity uses dot-separated reverse-DNS components.
    // packageName is retained as the field name during the Alpha migration.
    std::string packageName;
    // One source unit belongs to one module; empty means the root module.
    std::string modulePath;
    std::vector<std::string> sourceModules;
    std::vector<PackageUse> packageUses;
    bool isPackage = false;
    bool hasAnyKernel = false;
    std::vector<std::string> sourceFiles;
    std::vector<std::unique_ptr<Decl>> declarations;
};
