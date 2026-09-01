# Luna Standard-Library Design

> Document category: 0.3-oriented design and implementation boundary
> Current implementation baseline: Luna 0.3.0
> Status: Stage A and the Stage B host/allocator substrate are implemented; safe container
> and I/O adapters remain proposed

The standard library stays in the main repository until the package, resource,
Moon-container, and Runtime ABI contracts are stable. The 0.2.1 sources remain only in the
historical migration baseline; new owning-container APIs target the clean-break 0.3 compiler.
API sketches in this document describe semantics, not frozen 0.3 spelling.

## 1. Goals and non-goals

The first usable standard library must provide:

- console input/output and explicit byte/text I/O;
- files, paths, and common whole-file helpers;
- a safe growable heap container, `Vec<T>`;
- owned UTF-8 text, `String`, built on the same allocation model;
- `Option<T>`, `Result<T, E>`, concrete errors, and static conversions;
- the small comparison, conversion, iteration, and resource traits needed by those APIs.

It does not initially provide exceptions, a universal dynamically boxed `Error`, async I/O,
networking, threads, locale-aware formatting, custom allocators, non-UTF-8 paths, or a large
collection catalog. Those features require independent lifetime, ABI, and portability work.

## 2. Package and dependency boundary

The intended package graph is acyclic:

```text
org.luna.core                         # no package dependencies; shared-cell Runtime ABI only
       ^       ^
       |       |
org.luna.sys  |                       # raw Runtime/OS boundary; library-internal
       ^       |
       |       |
org.luna.alloc                         # Core + Sys allocator capability
       ^
       |
org.luna.std                           # Core + Alloc + Sys host services
```

In dependency notation:

- `core`: no package dependencies; Rc/Arc call only the fixed Luna shared-cell Runtime ABI;
- `sys -> core`;
- `alloc -> core + sys`;
- `std -> core + alloc + sys`.

`sys` is not a convenient user API and is not re-exported by the prelude. It preserves raw
status codes, handles, sizes, and ABI versions. `alloc` and `std` are the safe adapters. A
safe wrapper must never release a resource through a different allocator or host domain.

The workspace contains the dependency skeleton:

```text
stdlib/
├── core/   # org.luna.core
├── sys/    # org.luna.sys
├── alloc/  # org.luna.alloc
└── std/    # org.luna.std
```

## 3. Module inventory

| Package | Initial modules | Responsibility |
|---|---|---|
| Core | `prelude`, `option`, `result`, `error`, `convert`, `cmp`, `iter`, `resource`, `Rc`, `Arc` | Values/protocols plus shared containers in the fixed Luna Runtime allocation domain |
| Sys | `alloc`, `console`, `fs`, later `env`/`clock` | Versioned raw Runtime ABI imports and platform status |
| Alloc | `boxed`, `vec`, `string` | Luna-owned heap values and allocation failure |
| Std | `io`, `fs`, `path`, `fmt`, `env`, `process`, `time` | Safe host-facing APIs |

The initial prelude should remain small: `Option`, `Result`, `From`/`TryFrom`, `Drop`, `Clone`,
iterator traits, and optional ordinary `rc(value)`/`arc(value)` helpers. It should not silently import filesystem, console, process, or networking
operations. `Vec` and `String` may be re-exported by a future `std::prelude`, but their
canonical identities remain in `org.luna.alloc`.

## 4. Error and conversion model

Recoverable failure remains an ordinary `Result<T, E>`; process failure remains `panic`.
The library does not introduce exception unwinding or a global error registry.

The 0.3 direction is:

- `core::option::Option<T>` remains the canonical optional container;
- `core::result::Result<T, E>` has the canonical nominal Result identity;
- the compiler recognizes that exact Core Result identity for `?`, rather than every enum
  named `Result`;
- `From<S> for T` remains one-hop, static, unique, and auditable;
- `TryFrom<S>` and parsing traits return concrete error types;
- libraries return the narrowest useful error and applications define aggregate enums.

Core `Option<T>` now implements `is_some`, `is_none`, `unwrap`, `expect`, and
`unwrap_or`. The three extraction functions explicitly consume the Option and
therefore work for both Copy and affine payloads. `Some` transfers exactly one
payload, an unselected affine fallback in `unwrap_or` is cleaned exactly once,
and `None` preserves the panic boundary. This surface does not depend on mutable
slices or any Slot/Fragment decision.

Core `result` now provides ordinary source-defined consuming `unwrap`, `expect`,
`unwrap_err`, `expect_err`, and `unwrap_or` functions. They work with Copy and affine
payloads, transfer the selected payload once, and clean an unused fallback or consumed
error payload exactly once. Result construction, matching, identity, and `?` remain the
same compiler-supported contracts.

The clean-break identity migration is implemented: compiler materialization of
`Result<T, E>` uses `org.luna.core::result::Result`, and the compiler-known `From`
contract uses `org.luna.core::convert::From`. Their current syntax and lowering remain
compiler-supported; moving those surfaces to ordinary source declarations is separate
library work.

Core retains allocation-free value errors such as `BoundsError`, `UtfError`, `LayoutError`,
and `AllocError`. Std adds host errors. The initial I/O error should retain machine facts
without requiring a diagnostic allocation:

```text
IoError
├── kind: IoErrorKind
├── raw_code: Option<i64>
└── operation: IoOperation
```

`IoErrorKind` should initially include `NotFound`, `PermissionDenied`, `AlreadyExists`,
`InvalidInput`, `UnexpectedEof`, `Interrupted`, `WouldBlock`, `Unsupported`, and `Other`.
EOF from `Read::read` is `Ok(0)`, not an error. Human-readable platform text is formatted
lazily and is not error identity.

No-allocation errors are important: allocation failure itself must be representable without
constructing a `String` or `Vec`.

## 5. Allocation and `Vec<T>`

`Vec<T>` is a standard-library nominal type, not a compiler builtin. Its conceptual state is:

```text
Vec<T>
├── data: Luna-owned allocation for T
├── length: initialized element count
├── capacity: allocated element count
└── allocator domain: Global Luna allocator in the first release
```

The first release supports only the process-wide Luna allocator installed before Runtime
activation. It does not expose user-defined allocators or permit foreign memory adoption.
Allocator identity is part of the resource contract even if the compact runtime
representation does not store a per-value allocator pointer.

Required invariants:

- `length <= capacity`;
- exactly `[0, length)` is initialized;
- growth checks `size_of<T> * capacity` overflow and alignment;
- a failed reserve leaves the original vector unchanged;
- moving elements clears their initialization state before cleanup can observe them;
- Drop destroys every remaining initialized element exactly once, then deallocates through
  the original Luna allocator domain;
- zero-sized types and zero capacity never rely on dereferenceable null pointers;
- shared/mutable slices borrow the vector and prevent invalidating growth while borrowed.

The minimum semantic API is:

```text
Vec::new() -> Vec<T>
Vec::try_with_capacity(usize) -> Result<Vec<T>, AllocError>
len / capacity / is_empty
as_slice / as_mut_slice
get / get_mut -> Option<borrow>
try_push(T) -> Result<unit, AllocError>
push(T)                         # convenience; panics on allocation failure
pop() -> Option<T>
try_reserve(usize) -> Result<unit, AllocError>
truncate / clear
```

`insert`, `remove`, `retain`, `append`, `split_off`, custom allocators, and spare-capacity
APIs are follow-up work. Indexing may panic on an invalid index; `get`/`get_mut` are the
recoverable alternatives.

The existing `FromIterator` builder cannot report allocation failure. For 0.3, retain an
infallible `collect` convenience that panics on OOM and add a fallible builder/terminal
(`TryFromIterator`/`try_collect`) for recoverable allocation. Do not hide a fallible `push`
inside an allegedly infallible protocol.

### 5.1 Prerequisites for safe implementation

General `Vec<T>` implementation starts only after these contracts exist:

1. generic recursive Drop glue and exact-once cleanup;
2. stable move-out/initialization tracking for heap elements;
3. mutable-slice borrowing and growth invalidation rules;
4. layout overflow/alignment queries usable by library code;
5. a Runtime allocation operation that reports failure without dereferencing null;
6. release-domain facts in the resource contract;
7. fallible iterator collection semantics.

Prerequisites 1 and 4 through 6 now exist: generic nominal instances have compiler-derived
recursive Drop and exact-once field cleanup; Runtime ABI v1 exposes checked array layouts,
allocation-free `LunaAllocErrorV1`, transactional fallible allocation/reallocation, and the
Global Luna release-domain fact; `org.luna.sys::alloc` forwards them without decoding host
tables. Prerequisites 2, 3, and 7 intentionally remain at the 0.3 language boundary,
Core `Rc<T>`/`Arc<T>` are complete as the first ordinary owned-container vertical slice that
does not require element move-out; `Vec<T>` still waits for the remaining prerequisites.

Until then, fixed `array<T, N>` and borrowed `slice<T>` remain the safe container baseline.

### 5.2 Borrowed byte and text views

The final 0.3 syntax for a mutable slice is not frozen, but its behavior is. A byte view is a
non-owning `{data, length}` pair with source provenance:

- `slice<T>` is a shared loan and permits reads only;
- a mutable slice is one exclusive loan and permits in-place replacement with exact cleanup
  of the replaced value;
- moving an element out through a mutable view is unavailable until the type system can prove
  replacement or expose initialization state;
- neither view may outlive its source, and a zero-length view still retains its loan and
  provenance;
- capacity-changing Vec operations, including a reserve that reallocates, are rejected while
  any view exists; non-growing element mutation still follows shared-versus-exclusive loans;
- `slice<u8>` is arbitrary bytes. A borrowed text view is shared bytes plus a valid UTF-8
  invariant; mutable raw bytes never become text without validation.

Paths use the borrowed text contract at the safe boundary. Sys passes the same bytes and
explicit length to the host filesystem table. There is no implicit conversion to `cstr`, and
an adapter that needs a NUL-terminated platform path must check embedded NUL first.

## 6. Text

`String` belongs in Alloc and owns valid UTF-8. A borrowed `str`-like view is required for
ergonomic I/O and formatting; its final 0.3 spelling is a language decision. The semantic
split is fixed even if spelling changes:

- owned `String`: growable UTF-8 bytes using the Luna allocator;
- borrowed text view: `{bytes, length}` with a shared loan and UTF-8 invariant;
- `slice<u8>`: arbitrary bytes, not implicitly valid text;
- `cstr`: non-owning NUL-terminated FFI boundary, not a general string view.

Minimum operations are construction, byte/text views, `len_bytes`, `is_empty`,
`try_push_char`, `try_push_str`, UTF-8 validation, and conversion to/from `Vec<u8>` with a
concrete `UtfError`. Unicode normalization, grapheme segmentation, locale, and regex are not
initial requirements.

The current builtin `string` remains a 0.2 compatibility type. Its 0.3 migration must be an
explicit desugaring/compatibility step, not silent ABI equivalence with `alloc::String`.

## 7. I/O and files

### 7.1 Byte I/O protocols

Std should begin with byte-oriented, synchronous protocols:

```text
Read::read(&mut self, &mut [u8]) -> Result<usize, IoError>
Write::write(&mut self, &[u8]) -> Result<usize, IoError>
Write::flush(&mut self) -> Result<unit, IoError>
Seek::seek(&mut self, SeekFrom) -> Result<u64, IoError>
```

Library helpers provide `read_exact`, `read_to_end`, `read_to_string`, and `write_all` while
preserving partial-operation and `Interrupted` behavior. Text decoding is explicit.

`Stdin`, `Stdout`, and `Stderr` are ordinary host handles implementing these protocols. The
extended Runtime Console v1 contract supports stdout/stderr writes, flushes, and optional
stdin reads. The minimal Runtime default does not advertise input; an ordinary generated
application explicitly installs the native application profile, while an embedding host may
supply and retain its own table. Initial convenience APIs may include `print`, `println`, and
`read_line`, but their implementation must route through these protocols rather than create
a second console ABI.

The 0.3 workspace provides a deliberately narrow, explicitly typed `std::io`
surface so applications can use the host substrate before the 0.3 traits and owned String
syntax are frozen:

```text
stdout_write_text(cstr) / stdout_write_line(cstr) -> i32 status
stderr_write_text(cstr) / stderr_write_line(cstr) -> i32 status
stdout_write_i32(i32) / stdout_write_i32_line(i32) -> i32 status
stderr_write_i32(i32) / stderr_write_i32_line(i32) -> i32 status
stdout_flush() / stderr_flush() -> i32 status
read_line_lossy() -> borrowed cstr
parse_i32_or(cstr, fallback) -> i32
```

Raw byte read/write wrappers are also available with caller-provided buffers and error
records. `read_line_lossy` uses a thread-local 4096-byte buffer, is replaced by the next call
on that thread, truncates longer input, and cannot distinguish a blank line from EOF or host
failure. These limitations are encoded in the name and documentation. This compatibility
surface will not be treated as the future `Read`/`Write`/`Display`/`FromStr` contract.

Formatting is layered above `Write`. `Display`, `Debug`, and a formatter may be introduced
incrementally. The first usable release may support primitive values and strings only;
compile-time format-string validation is desirable but not a prerequisite for raw byte/text
output.

### 7.2 Filesystem

`std::fs` initially provides:

```text
File::open(path) -> Result<File, IoError>
File::create(path) -> Result<File, IoError>
OpenOptions::{read, write, append, truncate, create, create_new}
File::{read, write, flush, seek, metadata, sync_all, close}
fs::{read, read_to_string, write, exists, metadata, remove_file, create_dir, create_dir_all}
```

`File` owns an opaque host handle. Explicit `close` reports failure. Drop performs best-effort
close because Drop cannot return a Result; callers that need close durability must call
`flush`/`sync_all`/`close` explicitly.

Runtime ABI v1 now exposes an optional, versioned filesystem capability with
open/read/write/seek/flush, stat, close, and basic path operations. Calls use status plus
caller-owned out parameters; they do not return borrowed process-global error strings.
Handles are never exposed as Luna heap pointers.

The first path API accepts valid UTF-8 paths and exposes borrowed `Path` plus owned `PathBuf`.
Platform-native non-UTF-8/UTF-16-preserving `OsStr`/`OsString`, directory iteration, links,
permissions, and canonicalization are follow-up work. The limitation must be explicit rather
than silently replacing invalid path data.

## 8. Other necessary foundations

The following are needed, but not all belong in the first implementation slice:

1. **Core traits:** `Drop`, `Clone`, `Default`, `From`, `TryFrom`, `Eq`, `Ord`, `Hash`, and
   iterator protocols. Compiler cooperation must use exact Core identities.
2. **Owned resources:** ordinary Core `Rc<T>`/`Arc<T>` now exist; `Box<T>` is still needed
   alongside Vec. Cross-thread Arc entry points must wait for compiler-derived thread-safety
   sysmeta.
3. **Collections:** `HashMap`/`HashSet` after Vec, String, Eq, Hash, and panic/failure cleanup
   are stable. A deque is a later specialization, not a prerequisite.
4. **Host utilities:** `env`, `process`, and monotonic `time` after their Runtime capabilities
   are versioned. Process spawning is separate from process exit/arguments.
5. **Parsing and formatting:** numeric parsing, `FromStr`, `Display`, and `Debug`; formatting
   must write incrementally and avoid mandatory intermediate String allocation.
6. **Math:** host-independent numeric functions may live in Core when semantics are portable;
   target/libm-dependent operations belong behind Std/Sys.

Networking, async tasks, threading/synchronization, random-number sources, dynamic libraries,
serialization, regex, and full Unicode are later packages or milestones. They must not block
the first usable IO/Vec/error surface.

## 9. Implementation sequence

### Stage A: contracts and package skeleton

- implemented: central exact 0.3 Core identities for Result, From, Drop, Clone, iterators, and
  resources; compiler-materialized Result and From have switched once and retain no 0.2
  identity branch;
- implemented: `org.luna.sys` and `org.luna.alloc` dependency skeletons;
- implemented: append-only console-input and filesystem Runtime ABI capability contracts,
  including compatibility with previously published v1 prefixes;
- implemented: mutable byte/text view semantics; final 0.3 syntax remains deliberately open.

### Stage B: first usable vertical slice

The native application-host substrate is implemented: generated JIT/AOT entry points install
stdin/filesystem services without overriding an embedding host, with opaque handles, UTF-8
path validation, partial I/O, structured errors, metadata, seek, sync, and exact-once close.
The recoverable allocator substrate is also implemented, including overflow checks,
allocation-free errors, zero-size behavior, and failure-preserving realloc. `org.luna.sys`
now owns raw `console`, `fs`, and `alloc` forwarding wrappers, so Luna library code does not
decode C service tables. The safe Luna APIs below remain pending the explicit 0.3 language
surface.

- implemented as a narrow 0.3 adapter: explicitly typed stdout/stderr text and i32
  writes, flush, raw byte I/O, lossy line input, and fallback-based i32 parsing;
- implemented: ordinary nominal `Rc<T>`/`Arc<T>`, explicit `Clone`, recursive Drop callbacks,
  and Runtime ABI v1 retain/release;
- `IoError` and allocation-free error mapping;
- `Box<T>`, `Vec<T>`, and `String` over the Global Luna allocator;
- `try_push`, `try_reserve`, `write_all`, and basic whole-file helpers;
- JIT/AOT tests for success, allocation failure, early return, move-only elements, partial
  I/O, close behavior, and UTF-8 errors.

### Stage C: ordinary application surface

- stdin, File/OpenOptions, Path/PathBuf, metadata and directory creation;
- Display/Debug and checked format strings;
- fallible iterator collection into Vec/String;
- env, process arguments/exit, and monotonic time.

### Stage D: broader library

- HashMap/HashSet and richer text/path support;
- networking, concurrency, and async only after separate designs.

## 10. Acceptance criteria

A standard-library feature is complete only when it has:

- one documented owner package and no reversed dependency;
- explicit success, recoverable failure, panic, and Drop behavior;
- exact allocator/handle release domain;
- positive and negative ownership tests, including early return and move-only payloads;
- allocation-failure and partial-I/O tests where applicable;
- JIT/AOT parity and installed-tree coverage;
- a migration note when replacing a 0.2 compiler builtin.

This boundary deliberately favors a small coherent library over a broad API catalog before
the 0.3 syntax and resource model settle.
