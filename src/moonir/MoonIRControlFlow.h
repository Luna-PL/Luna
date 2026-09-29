#pragma once

#include "MoonIRExpressions.h"

namespace moon {

// Canonical control-flow tables are intentionally local-based rather than
// SSA-based. Table indices are the serialized identities; source names are
// diagnostic payload only. Regions and scopes describe structure and
// ownership, while successor edges remain the sole execution semantics.
struct LocalRecord {
    LocalId id;
    ScopeId scope;
    LocalKind kind = LocalKind::Binding;
    std::string name;
    TypeRef type;
    luna::ownership::Usage usage = luna::ownership::Usage::Copy;
    luna::ownership::Relation relation = luna::ownership::Relation::Owned;
};

// A sequential consuming-array state needs one runtime cursor rather than an
// initialization bit for every element. A guarded cleanup for element i runs
// only when nextUnread <= i. The verifier requires a complete constant-index
// cleanup set sharing one cursor, so this cannot be used as an arbitrary
// conditional destructor.
struct CleanupGuard {
    LocalId nextUnread;
    uint64_t elementIndex = 0;
};

struct CleanupRecord {
    CleanupId id;
    ScopeId scope;
    PlaceRef place;
    TypeRef type;
    CleanupKind kind = CleanupKind::Value;
    luna::ownership::CleanupAction action =
        luna::ownership::CleanupAction::None;
    std::optional<CleanupGuard> guard;
};

struct ControlEdge {
    BlockId target;
    // Canonical execution order, normally reverse declaration order for
    // every lexical scope exited by this edge.
    std::vector<CleanupId> cleanups;
};

struct SwitchEdge {
    uint32_t tag = 0;
    ControlEdge edge;
    std::vector<LocalId> bindings;
};

struct Terminator : Node {
    TerminatorKind kind = TerminatorKind::Invalid;
    // Branch condition, switch scrutinee, or return value. Jump/resume/discard/
    // unreachable leave this empty.
    std::unique_ptr<Expr> operand;
    TypeRef switchType;
    ControlEdge primary;
    ControlEdge secondary;
    std::vector<SwitchEdge> cases;
    std::vector<CleanupId> exitCleanups;
    // Present only for RuntimeSlot. The nominal declaration and frozen
    // argument record are explicit verifier inputs; `operand` is the packed
    // record, `primary` enters the compiler-owned continuation, and
    // `secondary` continues after dispatch when the chain completes or
    // discards that continuation.
    DeclarationRef runtimeSlot;
    TypeRef runtimeArgumentsType;
};

struct BasicBlock : Node {
    BlockId id;
    RegionId region;
    ScopeId scope;
    std::vector<std::unique_ptr<Stmt>> operations;
    Terminator terminator;
};

struct RegionRecord {
    RegionId id;
    RegionId parent;
    RegionKind kind = RegionKind::Lexical;
    ScopeId scope;
    BlockId entry;
    BlockId exit;
    std::vector<BlockId> blocks;
    SourceLocation location;
    // Present only for an inlined Fragment region. The declaration table owns
    // the nominal identity and frozen Fragment contract; regions do not
    // duplicate that semantic payload.
    DeclarationRef fragment;
    // Ordered entry bindings for a Fragment region. Their type, relation and
    // usage must match the frozen fragment parameter contract.
    std::vector<LocalId> parameters;
};

struct ScopeRecord {
    ScopeId id;
    ScopeId parent;
    RegionId region;
    std::vector<LocalId> locals;
    // Declaration order. An exiting edge executes these in reverse order.
    std::vector<CleanupId> cleanups;
    SourceLocation location;
};

// In-memory proof of a lexical Ref override. A disposable private codegen
// proof covers one normal exit or one early return; publication and wire
// representation remain unimplemented.
struct RuntimeRefApplyRecord {
    RegionId region;
    LocalId reference;
    DeclarationRef slot;
};

struct ControlFlowGraph {
    bool sealed = false;
    BlockId entry;
    RegionId rootRegion;
    ScopeId rootScope;
    std::vector<BasicBlock> blocks;
    std::vector<RegionRecord> regions;
    std::vector<ScopeRecord> scopes;
    std::vector<LocalRecord> locals;
    std::vector<CleanupRecord> cleanups;
    // Never serialized in Moon Container 0.3.
    std::vector<RuntimeRefApplyRecord> runtimeRefApplies;

    const BasicBlock* findBlock(BlockId id) const;
    const RegionRecord* findRegion(RegionId id) const;
    const ScopeRecord* findScope(ScopeId id) const;
    const LocalRecord* findLocal(LocalId id) const;
    const CleanupRecord* findCleanup(CleanupId id) const;
};

// Compiler-internal placement obligations for Ref context construction and
// release. Edge exits are innermost-first; entries are outermost-first.
// Return and unreachable terminals have no successor edge and are explicit.
struct RuntimeRefApplyFlowEdge {
    BlockId source;
    BlockId target;
    std::vector<RegionId> exits;
    std::vector<RegionId> enters;
};

struct RuntimeRefApplyFlowTerminal {
    BlockId block;
    TerminatorKind kind = TerminatorKind::Invalid;
    std::vector<RegionId> exits;
};

struct RuntimeRefApplyFlowPlan {
    // Indexed by canonical BlockId; each stack is outermost-first.
    std::vector<std::vector<RegionId>> activeByBlock;
    std::vector<RuntimeRefApplyFlowEdge> edges;
    std::vector<RuntimeRefApplyFlowTerminal> terminals;
};

std::optional<RuntimeRefApplyFlowPlan> planRuntimeRefApplyFlow(
    const ControlFlowGraph& graph, std::string& error);

} // namespace moon
