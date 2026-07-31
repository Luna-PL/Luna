# Luna 0.2 Alpha Error-Model Contract

> Document category: language contract and status reference
> Applies to: Luna 0.2.0-alpha
> Status: core semantics Frozen for Alpha; adapter/API portions are Implemented Experimental or Planned
> Normative status: core semantics are normative; internal layout and planned capabilities in the status table are non-normative
> Initial implementation audit: `d0ab31c` (2026-07-31)

This document defines the error semantics that 0.2 Alpha may rely on, separating unfinished
standard-library/external adapters from the core model. See
[architecture decisions D005/D006](../decisions.md) for rationale.

## 1. Failure categories

This baseline defines only two kinds of failure:

- recoverable failure: an ordinary `Result<T, E>` value;
- non-recoverable process failure: `panic(message)`.

Error handling does not introduce exception unwinding, implicit handlers, effect rows, a
runtime error-type registry, or a global `Error` base class. Fragments and slots are a
separate structured-control model; errors do not cross that boundary implicitly.

## 2. `Result<T, E>`

`Result<T, E>` is a Value-domain, structural-identity compiler builtin tagged union:

```luna
Ok(value)
Err(error)
Ok::<T, E>(value)
Err::<T, E>(error)
```

The current surface also provides `is_ok`, `is_err`, `unwrap`, `unwrap_err`, and
exhaustive `match`. These names are part of the 0.2 Alpha surface, not a promise that
every helper remains a compiler builtin forever.

The contract is:

- a result owns only the payload selected by its current tag;
- usage is the upper bound of the usage of `T` and `E`;
- a Copy Result may be matched normally;
- a move-only Result requires `match move` to transfer its payload;
- cleanup must read the tag and clean only the active payload once;
- a mismatched `unwrap`/`unwrap_err` variant is a panic, not recoverable failure.

The current inline ADT v1 layout is compiler/MoonIR Alpha ABI, not C FFI or permanent
cross-version ABI.

## 3. `?`

`value?` requires:

1. `value` is `Result<T, E>`;
2. the current ordinary function returns `Result<U, F>`;
3. `E == F`, or there is one unique direct `impl From<E> for F`.

The success path produces `T`. The error path must:

1. obtain the active `E`;
2. perform the same cleanup as explicit `return` for the current scope;
3. invoke the statically selected `From::from`, if needed;
4. construct `Err` in the outer `Result<U, F>`;
5. return from the current function.

An inner container must not be reused because two Results appear to have the same bit
representation. Conversion must not be searched at runtime, and multi-hop conversion chains
must not be found automatically.

`?` currently cannot be used in fragments. Fragment `return`/`abort` and host-function
return/cleanup ownership differ; code must explicitly match Result and choose fragment
control behavior.

## 4. `From<Source>`

The frozen principle is “static, unique, auditable conversion.” The current 0.2
implementation restricts it further:

- Source and Target are concrete types;
- there is one non-generic `from(Source) -> Target`;
- lookup is one-hop and exact by TypeId;
- a move-only Source must be received by an owning parameter;
- coherence/orphan rules require the impl package to own the trait, Source, or Target at
  the relevant legal identity boundary.

Generic `From` and additional explicit-call syntax remain experimental follow-up work. They
may extend the surface, but must not turn conversion into implicit runtime dispatch.

## 5. `panic` and `never`

In 0.2 Alpha, `panic(message)`:

- accepts `string` or `cstr`;
- writes a diagnostic to the Runtime stderr console and flushes;
- aborts the process;
- produces `never`, with an LLVM `unreachable` terminator;
- does not unwind the language stack or guarantee local Drop during unwinding.

Failures requiring predictable resource cleanup must use `Result`. A future task/runtime
boundary may add isolation, but the current process abort must not silently become exception
unwinding or recoverable control transfer.

## 6. Standard error layers

Core/Std do not define one information-destroying universal error enum:

- Core: host-independent value errors such as arguments, bounds, UTF, layout, and allocation;
- Std: host errors such as I/O, paths, and networking;
- boundary: FFI, Runtime, GPU, and plugin errors.

Library APIs should return the narrowest concrete error. Applications may define an aggregate
enum and connect it to `?` using precise static `From`.

`org.luna.core` currently materializes:

- `ErrorCode`
- `InvalidArgumentError`
- `BoundsError`
- `UtfError`
- `LayoutError`
- `AllocError`

These are nominal enums declared by the standard library, not new `TypeKind` values.
Variant order is fixed by the 0.2 inline ADT baseline; new variants must be appended and
their compatibility impact recorded.

## 7. C/Runtime boundary

Luna `Result`, enums, `string`, `rc`, and `arc` must not be current C ABI types.
A safe adapter should:

1. call the raw C/Runtime API;
2. capture status/errno before it is overwritten;
3. save stable domain/code fields;
4. copy diagnostic text when needed;
5. return an ordinary Luna `Result<T, BoundaryError>`.

Runtime ABI v1 provides a caller-owned `domain/code/message` snapshot. Machine control must
use domain/code; message is optional diagnostic text and may be omitted if allocation
fails. The legacy borrowed `last_error` pointer is compatibility-only and must not be stored
in a long-lived error value.

## 8. Status matrix

| Capability | Status | Commitment |
|---|---|---|
| `Result<T, E>` semantics | Frozen for Alpha | Ordinary values, active payload, usage/cleanup rules |
| `Ok`/`Err`/match | Frozen for Alpha | Current construction and exhaustive matching |
| `?` propagation after cleanup | Frozen for Alpha | Static direct conversion; not across fragments |
| `panic` process abort | Frozen for Alpha | No unwinding in 0.2 |
| `never` bottom type | Frozen for Alpha | Diverging expressions satisfy ordinary value positions |
| Concrete `From` | Implemented Experimental | One-hop, non-generic, unique static impl |
| Result inline ADT v1 | Internal Alpha ABI | Compiler/MoonIR internal; not C ABI |
| Core value errors | Implemented Experimental | Nominal enums; variant order constrained by 0.2 |
| Runtime snapshot ABI v1 | Frozen ABI v1 | C-compatible status/domain/code/message snapshot |
| Luna Runtime/FFI adapter | Planned | Not yet delivered as a complete language-layer API |
| Error source chain | Planned | No requirement for global boxing or dynamic base class |
| Task-local panic/capture | Planned | Outside 0.2 semantics |

## 9. Regression evidence

The current model is covered by:

- `tests/result_error_aot.cmake`
- `tests/result_extended_aot.cmake`
- `tests/fixtures/result_*.luna`
- `tests/fixtures/never_type.luna`
- `tests/fixtures/panic.luna`
- `tests/runtime_abi_test.cpp`
- `tests/runtime_gpu_error_test.cpp`
- `tests/jit_aot_extended_parity.cmake`

New error capabilities must cover success, failure, move-only cleanup, JIT/AOT parity, and
external ABI boundaries. Adding only a syntax-positive test is insufficient.

## 10. Compiler diagnostic codes

Diagnostics use `error[stage/code]`, for example:

```text
error[ownership/OWN0001]: use after move of 'buffer'
```

Codes support editors, CI, and documentation lookup; messages, source snippets, and
`help:` text may continue to improve.

| Prefix | Stage | Representative codes |
|---|---|---|
| `LEX` | Lexing | `LEX0001`: invalid character |
| `PAR` | Parsing | `PAR0001`: missing expected syntax element |
| `PKG` | Package loading | `PKG0001`: unreadable input; `PKG0003`: package-name mismatch |
| `SEM` | Type/semantic analysis | `SEM0001`: undefined name; `SEM0002`: type constraint; `SEM0003`: missing return; `SEM0101`: C ABI |
| `TRT` | Trait | `TRT0001`: trait constraint error |
| `OWN` | Ownership | `OWN0001`: use after move; `OWN0002`: use after free; `OWN0003`: borrow conflict; `OWN0004`: linear value not consumed; `OWN0101`: GPU in-flight; `OWN0201`: control-flow state mismatch |
| `CGN` | Code generation | `CGN0001`: invalid host IR; `CGN0101`: CUDA; `CGN0102`: ROCm |
| `DRV` | Driver/AOT | `DRV0001`: runtime missing; `DRV0002`: native linker failure |

Each stage's `9999` is a generic fallback code. New public subcodes must add regressions
and update this table.

## 11. Structured compiler diagnostics

`luna check --message-format=json` implements `luna.diagnostic` JSONL version 1.
The protocol sequence is `hello`, zero or more `diagnostic` records, then
`summary`. A diagnostic carries stable severity, phase, code, message, optional
primary span, labels, notes, and fixes. Current rendered `help:` text is exposed
as a note; structured fixes are reserved but not yet produced.

Disk-backed primary spans use normalized absolute paths, required UTF-8 byte
offsets, an exclusive end, and one-based line/column display aids. Diagnostics
without a disk location use `primary: null`. The hello record identifies the
long-lived language version, compiler source commit, build target, and protocol
capabilities so tools need not infer compiler identity from `0.2.0-alpha` alone.
