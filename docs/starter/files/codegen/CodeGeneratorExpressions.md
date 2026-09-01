# src/codegen/CodeGeneratorExpressions.cpp — Expression-Level LLVM Code Generation (Largest File, 1820 Lines)

## What This File Does

This file implements all expression-generation methods in `CodeGenerator` and the `generateExpr(Expr*)` dispatch entry point, covering literals, identifiers, field/index access, operators, variants/Result/records, allocation, ordinary and intrinsic calls, GPU built-ins, `try`, assignment, ownership expressions, lambdas, and closures.

For C++ readers: this is the "expression evaluator" of the entire LLVM backend — each `generateXxx` corresponds to one kind of AST node; after dispatch via `generateExpr`'s dynamic_cast, LLVM IR is emitted using the `mBuilder->Create*` family of methods. Among these, `generateCall` is the largest (~470 lines), handling various built-in runtime calls as well as indirect calls through function pointers and closures.

## Key Functions and Methods

**`llvm::Value* CodeGenerator::generateExpr(Expr* expr)` (overall dispatch, line 1819)**
- Runs a series of dynamic_casts against Expr subclasses and calls the matching generateXxx. If `generateBinary`/`generateUnary` return nullptr, it keeps trying the remaining types (short-circuit logic). Unrecognized types finally raise an error and return PoisonValue.

**Literal generators** (generateIntLiteral/FloatLiteral/StringLiteral/BoolLiteral/UnitLiteral/ArrayLiteral):
- `IntLiteral`: `ConstantInt::get(i32Ty, value, true)`. `FloatLiteral`: `ConstantFP::get(f64Ty, value)`. `StringLiteral`: `CreateGlobalString` → GEP to obtain the first character. `BoolLiteral`: i1 constant. `UnitLiteral`: returns i32 0 (unit has no runtime payload). `ArrayLiteral`: `UndefValue` array → element-wise CreateInsertValue.

**Access generators** (generateIdentifier/DynamicSelect/FieldAccess/SliceLength/Index):
- `Identifier`: resolves in the order canonical local (LocalId) → mLocals (name) → resolveFunction, and returns the loaded value.
- `DynamicSelect`: compares each candidate field by field against the metadata (integer/float/bool/string), building a `CreateSelect` chain to pick the function pointer; aborts if the match count is not 1.
- `FieldAccess`: uses ExtractValue for Record types; GEP + Load for struct pointer types.
- `SliceLength`: ExtractValue on field 1 (the length) of the slice.
- `Index`: performs a bounds check with `rt_array_index_or_abort` on arrays or slices (unless safety can be proven statically), then GEP + Load.

**Arithmetic generators** (generateBinary / generateUnary):
- `Binary`: `&&` and `||` use a short-circuit CFG (CondBr branches left/right); all other operations — add/subtract/multiply/divide/remainder/bitwise/compare — pick the appropriate Create method based on float or integer types.
- `Unary`: dispatches negation / logical not / bitwise not / dereference.

**Construction generators** (generateVariantConstruct/ResultConstruct/RecordLiteral/InitAllocation/HeapAlloc):
- `VariantConstruct`: looks up the index by variantName, MemCpys each field into the enum payload slot, and builds the tag+payload structure.
- `ResultConstruct`: a similar structure, with a bool tag.
- `RecordLiteral`: struct types go through pointer allocation plus stores at field offsets; Record types use UndefValue + InsertValue.
- `InitAllocation`: loads the canonical allocation pointer and stores initialization values one by one at field/element offsets.
- `HeapAlloc`: calls `rt_alloc` to obtain a pointer, then stores the constructor arguments to initialize it.

**`generateCall` (most complex, lines 660-1130)**:
- Checks iterator terminals, compiler/runtime intrinsics, GPU built-ins and printing before resolving verified global calls, indirect function calls, and closure calls.

**Other expressions**: `generateTry` (conditional branch on isOk → on the failure edge, packResultPayload + emitCleanups + CreateRet), `generateAssign` (compound-assignment expansion / array elements / fields / locals), `generateMove` (optionally updates a guarded cursor), `generateBorrow` (address-of / array elements / slice elements), `generateDeref` (Load), `generateAddrOf` (returns an alloca), `generateLambda` (emits a hidden function + generateControlFlowBody), `generateEnvLoad` (loads fields of the closure env structure), `generateMakeClosure` (emits a hidden function + builds the closure struct {code_ptr, captured...}).

## Relationship to Surrounding Files and Pipeline Stages

- It is the core of the **code generation stage** — expression-level LLVM generation — called by `generateControlFlowBody` in `CodeGeneratorControlFlow.cpp` (LetStmt/ExprStmt/terminal conditions). It is also called indirectly by `CodeGeneratorIterator.cpp` (map/filter callbacks in the pipeline) and `CodeGeneratorGpu.cpp` (evaluation of launch arguments).
- Dependencies: `CodeGeneratorRangeAnalysis.h` (safe-index proofs), `../core/TypeLayout.h` (layout offsets), and all members of `CodeGenerator.h`/`CGHelpers.h`.
- Callers: `CodeGeneratorControlFlow.cpp`, `CodeGeneratorIterator.cpp`, and `generateLambda`/`generateMakeClosure` themselves.

## Further Reading

1. `CodeGeneratorControlFlow.cpp` — how generateExpr is called from CFG operations.
2. `CodeGenerator.h` — the list of all `generateXxx` signatures.
3. `CodeGeneratorGpu.cpp` — the GPU kernel launch path in `generateLaunch`.
4. `CodeGeneratorIterator.cpp` — `generateIteratorTerminal` and `emitCallableInvocation`.

---

---
title: src/codegen/CodeGeneratorFunctions.cpp
path: src/codegen/CodeGeneratorFunctions.cpp
Stage: Code Generation (CodeGen)
Role: LLVM code generation of a single function body (entry / GPU initialization prologue)
Language: C++
---
