# src/sema/BodyAnalyzer.cpp — Body Analysis and Expression Type-Checking Implementation

> In one sentence: the complete implementation of `BodyAnalyzer` (~3,900 lines): body analysis of functions/structs/enums/traits/impls, type inference for statements and expressions, and specialized handling for declaration families, builtins, GPU launches, selectors, and more.

## What This File Does

This is the largest implementation file in Sema. It traverses and annotates the AST node by node; its core actions are "compute a type for every expression, establish type constraints for every statement, and validate the various semantic rules."

Coverage:

1. Declarations: `analyzeFunction` (including kernel validation), `analyzeStruct`, `analyzeEnum`, `analyzeTrait`, `analyzeImpl` (with special cases for From/FromIterator/Drop).
2. Statements: `analyzeStmt`/`analyzeBlock` plus dedicated methods (`analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`).
3. Expressions: `analyzeExpr` plus dedicated methods (`analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeLaunch`/`analyzeSelect`/`analyzeIntrinsicCall`/lambda/Variant/Record/Try).
4. Return paths: `statementAlwaysReturns`/`blockAlwaysReturns`.
5. Binding usage: `inherentUsageForInitializer`/`finalizeBindingUsage`.

## Key Structs, Classes, and Enums

- Anonymous-namespace helpers in the file: `hasLayoutDependentTypeParameter`, `genericDropLayoutDependsOnParameter` (determining whether a generic Drop's layout depends on its type parameters).
- Private members `CaptureFrame` (declared in the .h) and `mCaptureFrames` (record of lambda captures).
- Local structs inside methods (e.g., `VariantView` within `analyzeMatchStmt`, local variables for iterator protocol resolution).

## Key Functions and Methods

- `analyzeFunction(decl)`: saves/restores context; builds type-parameter bindings; `kernel` special case (cannot be extern/constexpr/generic, the first parameter must be `index: i32`, remaining parameters are limited to `&device_buffer<T>`); binds parameter contracts; analyzes `where` constraint predicates (must be bool); `analyzeBlock(body, currentReturnType)`; when there is no explicit return, constrains to `TyUnit`; every reachable path of a non-unit function must return (otherwise an error is reported); a kernel's return type must be unit.
- `analyzeStruct`/`analyzeEnum`: struct validates field type resolution; enum deduplicates variant names and checks for inline recursive layout (`reachesInlineType`).
- `analyzeTrait`: stores method signatures (including Self/typeParams bindings) into `mTraitMethods` and creates `FunctionDecl` proxies.
- `analyzeImpl`:
  - `From` special case: only a `from` method is allowed, its signature must be `from(source)->target`, and a move-only source requires an explicitly affine/linear parameter.
  - General: checks that all trait methods are present and no extra methods exist; runs `analyzeFunction` per method; compares parameter types, ownership contracts, and return types against the trait signature.
  - `FromIterator` special case: validates the three-method protocol — begin (no parameters, returns an `affine builder`)/push (`&mut builder`, `affine item`, returns unit)/finish (`affine builder`→`affine target`).
  - `Drop` special case: `&mut self`, returns unit, non-generic, no layout dependency; on success, marks `needsDrop`/`dropGlueSymbol` on every nominal type in `mDeclaredTypes`.
- `analyzeStmt(stmt, expectedReturn)`: host control constructs are forbidden inside kernels (slot/apply/resume/abort/await/free); dispatches by statement type: slot declaration/invocation → `mContext.analyzeSlotDecl/Invoke`, apply → `analyzeApply`, `resume()` must be inside a fragment and interceptors forbid resume, `abort()` must be inside a fragment, `await` requires an `Event` type, `let` → `analyzeLetStmt`, `return` → constrains the return value and the linearity contract, `if`/`while` → `requireBool` on the condition before analyzing the body, `match` → `analyzeMatchStmt`, `for` → `analyzeForStmt`, `expr` → `analyzeExpr`, `free` → analyzes the operand.
- `analyzeBlock`: `enterScope`/`enterConstScope`/`enterSlotScope` → iterates statements via `analyzeStmt` → exits the three scopes.
- `analyzeExpr(expr)`: first checks literals (Int→`TyI32`, Float→`TyF64`, String→`TyString`, Bool→`TyBool`); `IdentifierExpr` → looks up `mFunctionFamilies` (more than one family reports an ambiguity and suggests using `select`) plus `lookupSymbol`; the rest are dispatched by type to dedicated methods (including lambda/binary/unary/call/field/index/array/record/variant/try/move/borrow/deref/address-of/block/if-expression/heap/select, etc.).
- `analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeIntrinsicCall`: resolution and type constraints for function calls, member methods (trait impl methods), iterators (protocol/recipe), and builtins/intrinsics. Public `symbols(Name)` infers Function/Fragment/Struct/Enum/Trait/MetadataSchema catalog kinds (generic nominals fail closed), and `symbol_set` terminals are folded here: `.all()` freezes SymbolId order, `.all::<M>()` validates metadata order, and query views support compile-time count/index plus statically expanded `for` loops without crossing ordinary call/return boundaries.
- `analyzeLaunch`: analyzes `launch kernel(...)` launches (thread count, arguments, events).
- `analyzeSelect`: declaration-family filtering for `select target with selector(...)` (working with `CompileTimeEvaluator` to evaluate selectors).
- `analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`/`analyzeLambdaExpr`/`analyzeVariantConstructExpr`/`analyzeRecordLiteralExpr`/`analyzeTryExpr`: dedicated methods split out of large branches (let-binding usage decisions, match variant/binding types, for iteration protocol/materialization, lambda captures and closure construction, variant construction/record literals, `try` error propagation).
- `inherentUsageForInitializer`/`finalizeBindingUsage`: validates conflicts between a `let` initializer's inherent usage (call result/copy/move source) and explicit declarations.
- `statementAlwaysReturns`/`blockAlwaysReturns`: reachable-return determination for return/`never` calls/if (with else, both branches return)/match (all arms)/apply (has a body)/block (used for the function return-completeness check).

## Relationship to Surrounding Files and Pipeline Stages

- Invoked through forwarding calls from `SemanticContext`'s `analyzeFunction`/`analyzeExpr`/`analyzeStmt`, etc.; it is the main driver of the "body analysis" pass after declaration collection.
- Accesses `SemanticContext` state through `BodyContextAccess`; delegates type inference to `TypeResolver` (`constrain`/`requireXxx`/`resolved`); delegates compile-time evaluation to `CompileTimeEvaluator`; forwards slot/apply/fragment statements to `ControlAnalyzer`.
- Consumes the `mImpls`/`mTraits`/`mFromConversions`/`mFromIteratorImplementations`/`mDeclaredTypes` registered by `DeclarationCollector`.
- The populated AST fields (`resultType`/`inferredType`/`resolvedSymbolName`/`compileTimeValue`, etc.) are finalized by `TypeResolver::materializeInferredTypes` and then handed to MoonIR.
- The kernel boundary check (`mInKernel`) ensures device code contains only DeviceMemory operations, preventing host constructs from leaking into HSACO.

## Further Reading

- `BodyAnalyzer.h` (interface and private method declarations).
- `TypeResolver.h/.cpp` (type constraints and finalization).
- `ControlAnalyzer.h/.cpp` (slot/apply forwarding).
- `CompileTimeEvaluator.h/.cpp` (reflection and compile-time evaluation).


---

---
kind: source-file-guide
module: sema
source: src/sema/BodyAnalyzer.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's function body and expression analysis
---

# src/sema/BodyAnalyzer.h — Body Analyzer (BodyAnalysis Implementation)

> In one sentence: `BodyAnalyzer` implements `BodyAnalysis`: analyzing the bodies of functions/structs/enums/traits/impls, as well as type inference and validation for all statements and expressions.

## What This File Does

`BodyAnalyzer` is the largest single component in Sema: it walks function bodies, struct fields, enum variants, trait method signatures, and impl method bodies, performing type checking and inference on every AST node. It covers:

- Body analysis of functions/structs/enums/traits/impls (`analyzeFunction`/`analyzeStruct`/`analyzeEnum`/`analyzeTrait`/`analyzeImpl`).
- Statement analysis (`analyzeStmt`/`analyzeBlock`): let/return/if/while/for/match/abort, etc.
- Expression analysis (`analyzeExpr`/`analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeLaunch`/`analyzeSelect`/`analyzeIntrinsicCall`, etc.).
- Return path analysis (`statementAlwaysReturns`/`blockAlwaysReturns`).
- Closure capture management (`CaptureFrame` stack).
- Binding usage decisions (`inherentUsageForInitializer`/`finalizeBindingUsage`).

C++ analogy: it corresponds to "function body type checking" in C++ semantic analysis — deriving a type for every expression (`analyzeExpr`), establishing type constraints for every statement (`analyzeStmt`), and performing specialized signature validation (the comparison of trait methods in `analyzeImpl`).

## Key Structs, Classes, and Enums

- `class BodyAnalyzer final : public BodyAnalysis`: the only public type; private members `BodyContextAccess mContext` and `vector<CaptureFrame> mCaptureFrames`.
- `struct CaptureFrame` (private, nested): lambda capture frame — `lambdaScopeDepth` (lambda scope depth), `captures` (list of free variable names, in order of first reference).
- The `BodyAnalysis` interface (in `SemanticContext.h`): `analyzeFunction/Struct/Enum/Trait/Impl`, `analyzeStmt/Block/Expr/Call/MemberCall/IteratorCall/Launch/Select`, `statementAlwaysReturns`/`blockAlwaysReturns`.
- Type aliases: `ConstValue`, `FromConversion`, `FromIteratorImplementation` (all from `BodyContextAccess`).

## Key Functions and Methods

(Semantics are described in the .cpp guide; responsibilities are listed here.)

- `analyzeFunction(decl)`: saves/restores context (returnType, inFunction, inKernel, etc.), enters the scope/const/slot scopes, `analyzeBlock` on the function body, checks return paths (`SawReturn` and `AlwaysReturns`), runs `checkUnresolved` on parameters/returns.
- `analyzeStruct(decl)`: validates field type resolution (`resolveTypeAST`).
- `analyzeEnum(decl)`: deduplicates variant names and checks for inline recursive layout (`reachesInlineType`).
- `analyzeTrait(decl)`: stores method signatures into `mTraitMethods` (including Self/typeParams bindings) and creates `FunctionDecl` proxies.
- `analyzeImpl(decl)`: verifies that all trait methods are present, runs `analyzeFunction` on each method body, and validates parameter/return types and contracts against the trait signature; `From` special case (shape of the `from` method); `FromIterator` special case (begin/push/finish protocol); `Drop` special case (`&mut self` shape, layout dependency, `needsDrop` marking).
- `analyzeStmt(stmt, expectedReturn)` / `analyzeBlock(block, expectedReturn)`: dispatches by statement type (`LetStmt`/`MatchStmt`/`ForStmt`, etc. are split into dedicated methods; `ReturnStmt`/`ExprStmt`/`IfStmt`/`WhileStmt`/`SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt`, etc. are handled directly).
- `analyzeExpr(expr)`: dispatches by expression type (literals, identifiers, binary/unary, `CallExpr` → `analyzeCall`, `LambdaExpr` → `analyzeLambdaExpr`, `TryExpr`/`VariantConstructExpr`/`RecordLiteralExpr`/`HeapAllocExpr`/`MoveExpr`/`BorrowExpr`/`DerefExpr`/`AddrOfExpr`/`SelectExpr`, each with its own dedicated method).
- `analyzeCall(call)`: type inference for function calls; `analyzeIntrinsicCall` checks builtin functions; `analyzeMemberCall`/`analyzeIteratorCall` handle member method and iterator calls respectively.
- `analyzeLaunch(launch)`: analysis of GPU kernel launches (`launch kernel(...)`).
- `analyzeSelect(selection)`: analysis of `select target with ...` declaration-family filtering.
- Private methods: `analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`/`analyzeLambdaExpr`/`analyzeVariantConstructExpr`/`analyzeRecordLiteralExpr`/`analyzeTryExpr`/`analyzeIntrinsicCall` (large branches split out into their own methods).
- `inherentUsageForInitializer`/`finalizeBindingUsage`: binding usage decisions (inheritance of a `let` initializer's type and `move` source).
- `statementAlwaysReturns`/`blockAlwaysReturns`: recursively check return paths (completeness of if/match/while/for/return/abort).

## Relationship to Surrounding Files and Pipeline Stages

- Invoked through forwarding calls from `SemanticContext`'s `analyzeFunction`/`analyzeExpr`/`analyzeStmt`, etc.
- Accesses nearly all of `SemanticContext`'s state through `BodyContextAccess` (symbol tables, types, impls, traits, etc.).
- Consumes the registration results of `DeclarationCollector` (`mImpls`/`mTraits`/`mDeclaredTypes`/`mFromConversions`/`mFromIteratorImplementations`).
- Compile-time evaluation triggered during analysis (metadata, constraints, reflection) is delegated to `CompileTimeEvaluator` (forwarded through `SemanticContext`).
- Slot/fragment-related statements (`SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt`) are forwarded to `ControlAnalyzer`.
- Type inference goes through `TypeResolver` (`constrain`/`requireXxx`/`resolved`, etc.).

## Further Reading

- `BodyAnalyzer.cpp` (implementation).
- `SemanticContext.h` (the `BodyAnalysis` interface).
- `TypeResolver.cpp` (type constraints).
- `ControlAnalyzer.cpp` (slot/fragment statement forwarding).
- `CompileTimeEvaluator.cpp` (compile-time evaluation).


---

---
kind: source-file-guide
module: sema
source: src/sema/CompileTimeEvaluator.cpp
lang: en
audience: Readers familiar with C/C++ who want to read Luna's compile-time evaluation implementation
---
