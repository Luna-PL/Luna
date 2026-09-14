#pragma once

#include "../core/TypeSystem.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace moon {

inline constexpr uint32_t FormatMajor = 0;
inline constexpr uint32_t FormatMinor = 3;

// Every semantic type edge in sealed MoonIR is a stable table reference.
// An empty reference represents the absence of a type, never an unresolved
// frontend object.
using TypeRef = luna::types::TypeId;
using TypeRefVec = std::vector<TypeRef>;
using SymbolRef = luna::identity::SymbolId;
using ContractRef = luna::identity::ContractId;

// Every executable reference names both the declaration and the semantic
// contract expected by the use site. Linkage names are declaration-table
// payload, not executable-node references.
struct DeclarationRef {
    SymbolRef symbol;
    ContractRef contract;

    bool empty() const { return symbol.empty() && contract.empty(); }
    bool complete() const { return !symbol.empty() && !contract.empty(); }
    bool operator==(const DeclarationRef& other) const {
        return symbol == other.symbol && contract == other.contract;
    }
    bool operator!=(const DeclarationRef& other) const {
        return !(*this == other);
    }
};

using DeclarationRefVec = std::vector<DeclarationRef>;

inline constexpr uint32_t InvalidTableIndex =
    std::numeric_limits<uint32_t>::max();

template <typename Tag>
struct TableRef {
    uint32_t value = InvalidTableIndex;

    bool empty() const { return value == InvalidTableIndex; }
    bool operator==(const TableRef& other) const {
        return value == other.value;
    }
    bool operator!=(const TableRef& other) const {
        return !(*this == other);
    }
};

struct BlockTag;
struct RegionTag;
struct ScopeTag;
struct LocalTag;
struct CleanupTag;
using BlockId = TableRef<BlockTag>;
using RegionId = TableRef<RegionTag>;
using ScopeId = TableRef<ScopeTag>;
using LocalId = TableRef<LocalTag>;
using CleanupId = TableRef<CleanupTag>;

enum class RegionKind : uint8_t {
    Function,
    Lambda,
    Fragment,
    Continuation,
    Lexical,
    Loop,
    MatchArm,
    Apply,
};

enum class LocalKind : uint8_t {
    Parameter,
    Binding,
    Pattern,
    Synthetic,
    Allocation,
};

enum class CleanupKind : uint8_t {
    Value,
    Allocation,
};

enum class TerminatorKind : uint8_t {
    Invalid,
    Jump,
    Branch,
    Switch,
    Return,
    Resume,
    Abort,
    Unreachable,
};

enum class ProjectionKind : uint8_t {
    Field,
    ConstantIndex,
    DynamicIndex,
    Dereference,
};

struct PlaceProjection {
    ProjectionKind kind = ProjectionKind::Field;
    // Canonical field ordinal or constant element index.
    uint64_t index = 0;
    // Dynamic indices are first materialized as canonical locals.
    LocalId dynamicIndex;

    bool operator==(const PlaceProjection& other) const {
        return kind == other.kind && index == other.index &&
               dynamicIndex == other.dynamicIndex;
    }
};

struct PlaceRef {
    LocalId root;
    std::vector<PlaceProjection> projections;

    bool empty() const { return root.empty(); }
    bool operator==(const PlaceRef& other) const {
        return root == other.root && projections == other.projections;
    }
};

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
};

using DeclarationKind = luna::sysmeta::DeclarationKind;

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
    TypeRef type;
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

struct TypeFieldRecord {
    std::string name;
    TypeRef type;
};

struct TypeVariantRecord {
    std::string name;
    TypeRefVec fields;
};

// This is the complete, pointer-free type payload used by both in-memory
// sealed MoonIR and its future serialized form. `referencedTypeIds` is a
// canonical traversal index; the named edge fields preserve enough structure
// for an independent reader or backend to reconstruct the type graph.
struct TypeRecord {
    luna::types::TypeId id;
    luna::types::ShapeId shapeId;
    luna::identity::AbiLayoutId abiLayoutId;
    luna::types::TypeDomain domain = luna::types::TypeDomain::Value;
    luna::types::IdentityMode identityMode = luna::types::IdentityMode::Structural;
    TypeKind kind = TypeKind::Unknown;
    luna::sysmeta::Facts sysmeta;
    std::string displayName;
    std::string sourceName;
    std::string declarationLinkageName;
    std::string nominalDeclarationId;
    std::vector<std::string> typeParameterNames;
    TypeRefVec typeArgumentIds;
    TypeRef innerTypeId;
    uint64_t arrayLength = 0;
    bool isMutable = false;
    TypeRefVec parameterTypeIds;
    TypeRef returnTypeId;
    std::vector<luna::ownership::Contract> parameterContracts;
    luna::ownership::Contract returnContract;
    bool isMultiShot = false;
    ContinuationKind continuationKind = ContinuationKind::Context;
    IteratorMode iteratorMode = IteratorMode::Copy;
    std::vector<TypeFieldRecord> fields;
    std::vector<TypeFieldRecord> capturedFields;
    std::vector<TypeVariantRecord> variants;
    int inferenceId = -1;
    std::string canonicalType;
    std::string canonicalShape;
    std::string canonicalAbiLayout;
    uint32_t layoutAbiVersion = 0;
    uint64_t valueSize = 0;
    uint64_t valueAlignment = 1;
    std::string abiLayout;
    DeclarationRef dropGlue;
    // Immediate graph edges make the frozen table independently traversable;
    // Moon readers never need a frontend-owned Type pointer to discover the
    // complete closed type graph.
    std::vector<luna::types::TypeId> referencedTypeIds;
};

struct DeclarationRecord {
    std::string id;
    std::string familyId;
    luna::identity::SymbolId symbolId;
    luna::identity::ContractId contractId;
    std::string sourceName;
    std::string linkageName;
    DeclarationKind kind = DeclarationKind::Function;
    Retention retention = Retention::CompileTime;
    std::vector<MetadataInstance> metadata;
    TypeRef type;
    luna::sysmeta::Facts sysmeta;
    DeclarationRef dropGlue;
    std::string canonicalContract;
    SourceLocation location;
};

std::string canonicalAbiLayout(const TypeRecord& type);
std::string canonicalContract(const DeclarationRecord& declaration);

struct Param {
    std::string name;
    bool isLinear = false;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
    TypeRef type;
};

struct CleanupObligation {
    std::string place;
    luna::ownership::CleanupAction action = luna::ownership::CleanupAction::None;
    luna::types::TypeId typeId;
};

struct Expr;
struct Stmt;
struct BlockStmt;
struct ControlFlowGraph;

} // namespace moon
