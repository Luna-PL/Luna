#pragma once

#include "MoonIRTypes.h"

namespace moon {

struct Stmt : Node {};

struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

struct LetStmt : Stmt {
    std::string name;
    LocalId local;
    bool isConst = false;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    // Construction may omit this and derive it from the frozen type. A
    // canonical operation always records the final local relation explicitly;
    // fragment parameter bindings use it to distinguish views from transfers.
    std::optional<luna::ownership::Relation> relation;
    TypeRef type;
    std::unique_ptr<Expr> initializer;
    bool materializesIteratorRecipe = false;
    bool materializedIteratorOwnsSource = false;
    TypeRef materializedIteratorSourceType;
};

// Defines one raw owning allocation. The storage has no initialized value
// yet, so only its backing allocation cleanup is active. InitAllocationExpr
// later consumes this identity after every initializer has succeeded.
struct AllocateStmt : Stmt {
    LocalId local;
    TypeRef allocatedType;
    HeapStorageKind storage = HeapStorageKind::Unique;
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

struct MatchArm : Node {
    std::string variantName;
    uint32_t variantIndex = 0;
    std::vector<std::string> bindings;
    TypeRefVec bindingTypes;
    std::vector<luna::ownership::Usage> bindingUsages;
    std::unique_ptr<BlockStmt> body;
};

struct MatchStmt : Stmt {
    std::unique_ptr<Expr> scrutinee;
    TypeRef matchedType;
    std::vector<MatchArm> arms;
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> body;
};

struct ForStmt : Stmt {
    std::string varName;
    luna::ownership::Usage bindingUsage =
        luna::ownership::Usage::Copy;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<BlockStmt> body;
    TypeRef elementType;
    DeclarationRef protocolNext;
    TypeRef protocolIteratorType;
    TypeRef protocolOptionType;
    uint32_t protocolNoneVariant = 0;
    uint32_t protocolSomeVariant = 0;
    DeclarationRef protocolInto;
    TypeRef protocolInputType;
    std::string protocolStateName;
    bool protocolStateNeedsCleanup = false;
    luna::ownership::CleanupAction protocolStateCleanup =
        luna::ownership::CleanupAction::Deallocate;
    std::string recipeStateName;
    TypeRef recipeSourceType;
};

struct FreeStmt : Stmt {
    std::unique_ptr<Expr> operand;
    luna::ownership::CleanupAction action =
        luna::ownership::CleanupAction::Deallocate;
    bool isImplicit = false;
};

struct SlotDeclStmt : Stmt {
    std::string name;
    FragmentKind acceptedKind = FragmentKind::Interceptor;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    std::vector<Param> params;
    std::string defaultFragment;
    DeclarationRef defaultFragmentRef;
    TypeRef structuralType;
};

struct SlotInvokeStmt : Stmt {
    std::string name;
    FragmentKind acceptedKind = FragmentKind::Interceptor;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    std::vector<std::unique_ptr<Expr>> args;
    std::unique_ptr<BlockStmt> continuation;
    bool isImplicitCapture = false;
    std::vector<Param> interfaceParams;
    std::vector<std::string> resolvedParamNames;
    std::string defaultFragment;
    DeclarationRef defaultFragmentRef;
    TypeRef structuralType;
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
    DeclarationRef fragmentRef;
    std::unique_ptr<BlockStmt> body;
};

struct Expr : Node {
    TypeRef type;
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

// Canonical zero-sized value produced after a unit-valued control expression
// has been expanded into CFG blocks. It carries no execution semantics of its
// own; the surrounding edges already encode those effects.
struct UnitExpr : Expr {};

struct IdentifierExpr : Expr {
    std::string name;
    LocalId local;
    DeclarationRef declaration;
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
    TypeRefVec typeArgs;
    DeclarationRef calleeRef;
    bool returnsLinear = false;
    luna::ownership::Usage returnUsage = luna::ownership::Usage::Copy;
    TypeRef intrinsicType;
    TypeRef iteratorInputType;
    TypeRef iteratorOutputType;
    IteratorOp iteratorOp = IteratorOp::None;
    std::string iteratorRecipeStateName;
    TypeRef iteratorRecipeSourceType;
    TypeRef iteratorCollectTargetType;
    TypeRef iteratorCollectBuilderType;
    DeclarationRef iteratorCollectBegin;
    DeclarationRef iteratorCollectPush;
    DeclarationRef iteratorCollectFinish;
    std::optional<ConstantValue> compileTimeValue;
};

struct LaunchExpr : Expr {
    std::string kernelName;
    DeclarationRef kernelRef;
    std::unique_ptr<Expr> threads;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::pair<std::string, bool>> inFlightResources;
};

struct VariantConstructExpr : Expr {
    std::string typeName;
    std::string variantName;
    std::vector<std::unique_ptr<Expr>> args;
    TypeRef constructedType;
};

// Ordinary Result data construction used after `?` has become explicit CFG.
// Err is tag 0 and Ok is tag 1, matching the frozen Result ABI and Switch.
struct ResultConstructExpr : Expr {
    bool isOk = false;
    std::unique_ptr<Expr> payload;
};

struct FieldAccessExpr : Expr {
    std::unique_ptr<Expr> object;
    std::string field;
};

struct IndexExpr : Expr {
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};

// A fundamental projection of the runtime length carried by a slice value.
// This is not an iterator operation: canonical CFG construction also uses it
// wherever a slice bound cannot be represented by an array-length constant.
struct SliceLengthExpr : Expr {
    std::unique_ptr<Expr> slice;
};

struct ArrayLiteralExpr : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
    TypeRef elementType;
};

struct RecordLiteralExpr : Expr {
    struct Field {
        std::string name;
        std::unique_ptr<Expr> value;
    };
    // Source order is retained for evaluation; `type->fields` owns canonical
    // identity/layout order.
    std::vector<Field> fields;
};

struct HeapAllocExpr : Expr {
    std::unique_ptr<Expr> initializer;
    TypeRef allocatedType;
    HeapStorageKind storage = HeapStorageKind::Unique;
};

// Atomically commits already-evaluated initializer values into raw allocated
// storage and returns the initialized owning value. `index` is the frozen
// product field ordinal, or zero for scalar allocation.
struct InitAllocationExpr : Expr {
    struct Element {
        uint32_t index = 0;
        std::unique_ptr<Expr> value;
    };
    LocalId allocation;
    TypeRef allocatedType;
    HeapStorageKind storage = HeapStorageKind::Unique;
    std::vector<Element> elements;
};

struct TryExpr : Expr {
    std::unique_ptr<Expr> operand;
    TypeRef resultType;
    TypeRef propagatedResultType;
    TypeRef valueType;
    TypeRef errorType;
    TypeRef propagatedErrorType;
    DeclarationRef errorConversion;
    std::vector<CleanupObligation> cleanups;
};

struct MoveExpr : Expr {
    std::unique_ptr<Expr> operand;
    // Non-empty only for a canonical sequential transfer from a dynamically
    // indexed array element. The transfer atomically sets nextUnread to
    // index + 1, matching the guarded tail cleanup owned by the source array.
    LocalId nextUnread;
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
    ~LambdaExpr() override;

    std::vector<Param> params;
    TypeRef returnType;
    // Construction-only structured input. Canonical CFG construction consumes
    // this body and leaves exactly one executable meaning in controlFlow.
    std::unique_ptr<BlockStmt> body;
    std::unique_ptr<ControlFlowGraph> controlFlow;
    TypeRef closureType;
    std::vector<std::string> captures;
    std::string identitySuffix;
    // Synthetic environment parameter of a capturing lambda. Non-empty only
    // when the lambda captures; canonical construction declares it as a
    // Parameter local whose type is the closure type (C016 CL007).
    std::string envParamName;
};

// Constructs a canonical closure value from a lambda executable plus the
// materialized captured environment values (C016 CL007). The lambda retains
// its own body/CFG; the closure value is { code, env_fields... }.
struct MakeClosureExpr : Expr {
    std::unique_ptr<LambdaExpr> lambda;
    // Captured environment values in canonical captured-field order. In the
    // structured representation these are identifier references; canonical
    // construction binds them to LocalIds before sealing.
    std::vector<std::unique_ptr<Expr>> capturedValues;
};

// Reads a typed environment field from the closure's implicit environment
// parameter (C016 CL007). `fieldIndex` is the canonical captured-field ordinal.
struct EnvLoadExpr : Expr {
    LocalId envLocal;
    uint64_t fieldIndex = 0;
};

struct AssignExpr : Expr {
    Operator op = Operator::Assign;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

} // namespace moon
