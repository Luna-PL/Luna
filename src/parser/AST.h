#pragma once

#include "../lexer/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <cstdint>

// ─── Forward declarations ────────────────────────────────────────────
struct Type;
using TypePtr = std::shared_ptr<Type>;
using TypeVec = std::vector<TypePtr>;

struct Param;
struct Expr;
struct Stmt;
struct BlockStmt;
struct TypeAST;

enum class FragmentKind { Interceptor, Context };
enum class FragmentCardinality { Once, Many };

// Versions are language values rather than strings so version ordering is
// semantic (`1.10.0` is newer than `1.2.0`) and never lexicographic.
struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator==(const SemanticVersion& other) const {
        return major == other.major && minor == other.minor && patch == other.patch;
    }
    bool operator<(const SemanticVersion& other) const {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return patch < other.patch;
    }
    std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." +
               std::to_string(patch);
    }
};

struct VersionTag {
    std::string name;
    SemanticVersion version;
};

// A selector without `version` means the latest declaration in that tag.
struct VersionSelector {
    std::string tag;
    std::optional<SemanticVersion> version;
};

inline std::string versionedDeclarationName(
    const std::string& name, const std::optional<VersionTag>& tag) {
    if (!tag) return name;
    return name + "__" + tag->name + "__v" +
           std::to_string(tag->version.major) + "_" +
           std::to_string(tag->version.minor) + "_" +
           std::to_string(tag->version.patch);
}

// ─── AST Node base ───────────────────────────────────────────────────
struct ASTNode {
    virtual ~ASTNode() = default;
    std::string sourcePath;
    int line = 0;
    int col = 0;
};

// Traits participate in constraint solving and coherence, so a trait use must
// retain its selector and eventually the exact declaration identity chosen by
// semantic analysis.  A plain string is not sufficient once traits are
// versioned.
struct TraitRef : ASTNode {
    std::string name;
    std::optional<VersionSelector> versionSelector;
    std::string resolvedTraitId;
};

struct WhereClause {
    std::string typeParam;
    TraitRef trait;
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
    std::optional<VersionSelector> versionSelector;
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
    std::unique_ptr<TypeAST> typeAnnotation; // optional
    std::unique_ptr<Expr> initializer;
};

struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value; // nullptr for unit return
    // Filled by ownership checking.  These are non-linear heap values that
    // remain live on this exact return path and therefore need runtime
    // cleanup before the LLVM `ret` is emitted.
    std::vector<std::string> autoFrees;
};

struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> thenBlock;
    std::unique_ptr<Stmt> elseBranch; // BlockStmt or IfStmt
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
    std::optional<VersionSelector> defaultFragmentSelector;
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
    std::optional<VersionSelector> defaultFragmentSelector;
    std::string resolvedDefaultFragmentName;
    TypePtr structuralType;
};

struct ResumeStmt : Stmt {};
struct AbortStmt : Stmt {};

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
    std::optional<VersionSelector> fragmentSelector;
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
    std::optional<VersionSelector> versionSelector;
    // Semantic analysis writes the hygienic LLVM symbol selected by `@tag`.
    std::string resolvedSymbolName;
    // `linear` is an ownership qualifier rather than an LLVM ABI type. Keep
    // it on calls so the ownership pass can enforce an owning FFI result even
    // though the resolved value type is still `raw<T>`.
    bool returnsLinear = false;
    std::optional<std::variant<int64_t, double, bool, std::string>> compileTimeValue;
};

// A launch is an expression so the returned event can be held in a linear
// binding and awaited explicitly. `threads` is the one-dimensional dispatch
// size used by the CPU simulator and by the initial GPU ABI.
struct LaunchExpr : Expr {
    std::string kernelName;
    std::optional<VersionSelector> kernelSelector;
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
    std::optional<VersionTag> versionTag;
};

struct AssignExpr : Expr {
    TokenKind op = TokenKind::Eq;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
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
    // Package declarations are private by default. An explicit export is the
    // only way a declaration becomes part of the package's public interface.
    bool isExported = false;
    std::optional<VersionTag> versionTag;
    // Versioned declarations share a source name but must never share an LLVM
    // linkage name.  Untagged declarations keep this empty and use `name`.
    std::string generatedSymbolName;
};

struct Param {
    std::string name;
    bool isLinear = false;
    std::unique_ptr<TypeAST> type;
    TypePtr inferredType; // Filled by constraint-based semantic analysis.
};

struct FunctionDecl : Decl {
    std::string name;
    bool isKernel = false;
    bool isExtern = false;
    bool isConstexpr = false;
    std::string abi;
    std::string linkName;
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    std::unique_ptr<TypeAST> returnType;
    // Preserves a direct `-> linear raw<T>` return annotation. Type lowering
    // intentionally erases `linear`, so ownership needs this separate bit.
    bool returnsLinear = false;
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
    std::vector<std::string> typeParams;
    std::vector<Variant> variants;
};

struct TraitDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<Param> traitParams;
    struct MethodSig {
        std::string name;
        std::vector<Param> params;
        std::unique_ptr<TypeAST> returnType;
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

struct Program : ASTNode {
    std::string packageName;
    bool isPackage = false;
    bool hasAnyKernel = false;
    std::vector<std::string> sourceFiles;
    std::vector<std::unique_ptr<Decl>> declarations;
};
