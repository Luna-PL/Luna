# Testing and Regressions

Luna's stable core uses CTest for semantic regressions. The suite covers both programs that
should pass and programs that should be rejected. It does not treat a non-zero return from
source-language `main` as a compilation failure; it checks the compiler's
`Program exited with code:` line or a structured diagnostic.

Core errors also have stable error codes. See the [error-model diagnostic
codes](reference/error_model.md#10-compiler-diagnostic-codes) for the format and current
public numbering.

## Running

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The production pipeline now always seals canonical CFG, so the ordinary CTest
command above is the canonical regression gate. Static single-shot interceptors
and contexts are positive cases; `context many`, dynamic slot/apply, and external
context callbacks are explicit rejection boundaries. A green matrix means both
the supported surface and deferred boundaries match their contracts.
The structured executable-body backend has been deleted. A focused unit test
also calls codegen directly and requires it to reject an unsealed body with the
canonical-boundary diagnostic.

Release-candidate builds should also promote project-owned warnings to errors:

```sh
cmake -S . -B build -DLUNA_STRICT_WARNINGS=ON
cmake --build build --parallel
```

Linux CI enables this gate in both the C++17 and C++23 build matrices. It applies only to
targets owned by the Luna repository, so third-party warnings from LLVM or system headers
are not misclassified as project regressions.

`luna.moon-container` exercises the M005 untrusted-byte boundary independently
from frontend lowering. It checks deterministic output, file round trips,
required and optional sections, magic/version/header integrity, section order,
compression rejection, SHA-256 corruption detection, and parser resource
limits. `luna.moon-container-model` covers all eight canonical model sections,
malformed opcodes/truncation/depth bounds, failure atomicity, and verifier
handoff. It also runs deterministic raw-bit, truncation, and authenticated
payload mutations; rejected inputs must not publish partial state, while any
mutation that remains valid must re-encode byte-for-byte. `luna.moon-container-cli`
freezes package-only, deterministic `-t moon` behavior. The separate
`luna.moon-container-oracle` Python parser links no Luna reader code and consumes
every field of all eight sections, including complete CFG and expression payloads,
from two real CLI builds. `luna.moonir-canonical` serializes, reloads, and JITs a real
frontend product to compare behavior. That production fixture contains both a
generic recipe and its concrete instance, proving that projection drops the
recipe/unresolved types while retaining executable monomorphized code. It also
contains an unreachable concrete function and a two-edge call chain, proving
that dead declarations disappear while transitive callees remain executable.
Its direct verifier negatives also reject both a missing literal `TypeRef` and
an integer literal forged with a boolean type.

`luna.moon-runtime` tests the EV001–EV004 state machine through the public
`runtime/Evolution.h` surface. It requires strict
verify-resolve-initialize staging order, one-use same-runtime safe points,
failure atomicity, typed SymbolId/ContractId/kind/flag compatibility, pinned old
references, switchable new references, retained code leases, and rollback.
Its load-once cases require identical content to reuse the first pinned
generation, reject different content, and keep exactly one retained lease.
`luna.runtime-descriptor-v1` independently validates the installed
C-compatible registry ABI, exact SymbolId/ContractId/kind/flag lookup,
strict ordering, typed metadata, callable-kind constraints, and version
rejection. The canonical Moon loader test additionally resolves the emitted
registry from ORC and cross-checks its TypeIds, metadata payloads, and callable
classification against the verified Container before publication. It invokes
a private retained function only through that descriptor-backed binding. The cost-boundary
test requires descriptor/registry v1 types and stable identities only for a
runtime-retained program and proves that a static query emits none of them.
Four concurrent readers cross 1000 generation transitions and must observe
only complete identity/implementation pairs from all activated generations.
The canonical round-trip test also loads a real verified Moon Container once,
proves that an identical request does not materialize another ORC generation,
and executes two compatible generations as
60 -> 13 -> 60 across activation and rollback. The pinned first entry remains 60,
the replacement initializer executes the resolved 13 entry before publication,
both code leases remain retained, and initializer failure or a corrupted container
must leave the active generation unchanged.

`luna.cffi-artifact` builds a platform shared library and C header from a real
library package, then uses the system C compiler in strict C11 mode to compile
and run an independent consumer. It also requires application, empty-C-export,
ordinary Luna export, standalone-build, and C-export-as-Native cases to fail
without producing the primary artifact. The same target-matrix test
requires Native applications to use `build/native`, rejects standalone Native
builds and missing manifest kinds, and executes the resulting package artifact.
Legacy single-file AOT regressions use `aot_package_fixture.cmake` to stage
temporary application packages without creating a production bypass.

`luna.native-artifact` builds a real Native library and independently invokes
the offline trust verifier. It requires the dedicated embedded proof section,
an exact explicit trust record, package/target/compiler identity, and rejects
post-link byte tampering, an empty trust store, and proof-free native binaries.
The candidate `.trust` file is evidence for an installer, never an implicit
adjacent-file trust path. A separate Python SHA-256/framing oracle does not
reuse compiler code, and an independent raw platform consumer calls the real
typed export and requires the result `42`. The production verifier and oracle
separately reconstruct the final ELF/Mach-O/PE dependency table; a forged
dependency digest must fail before trust-store lookup.
The consumer obtains the callable through
`luna_native_library_descriptor_v1`, validates every registry row, rebuilds the
canonical export digest from SymbolId/ContractId/kind/flags/linkage, and only
then calls the descriptor entry. It does not use a guessed raw function name.
The production loader separately captures immutable/private staging, verifies
and loads that same image, and exposes exact SymbolId+ContractId lookup. The
test replaces the original path with an untrusted implementation returning
`13` after verification and still requires `42`; it also forges a matching
trust row for a changed proof export digest and requires registry rejection.
The load-once adapter additionally resolves the callable with an exact
SymbolId/ContractId/function/callable requirement, skips another platform load
for identical trusted content, and rejects a second trusted image with the same
Package ID before publication.
The same independent consumer drives two trusted Native generations: a pinned
first binding remains `42`, the switchable binding becomes `13` after second
activation, and rollback retargets only the switchable binding to `42`.

Moon Container coverage-guided fuzzing is opt-in and requires Clang/libFuzzer:

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLUNA_ENABLE_FUZZING=ON -DLUNA_STRICT_WARNINGS=ON
cmake --build build-fuzz --target moon-container-fuzz moon-container-fuzz-corpus
ctest --test-dir build-fuzz -L fuzz --output-on-failure
```

The corpus generator preserves coverage-discovered files while refreshing 11
stable seeds, including a real CLI product and independently encoded framing
cases. The custom mutator alternates raw mutations with authenticated directory
and payload mutations, allowing SHA-valid inputs to reach the model decoder.
For longer local exploration, invoke `moon-container-fuzz` with
`-max_total_time=<seconds>`, the generated corpus directory, and
`tests/moon_container_fuzz.dict`.

On the current LLVM 22 shared-library build, Clang ASan reports an
alloc/dealloc mismatch during LLVM static initialization even for an empty
fuzzer linked to `libLLVM`. The fuzz CTest disables only that check and retains
all other ASan/UBSan checks; the separate full sanitizer matrix keeps the
default alloc/dealloc-mismatch check enabled.

Memory-safety and undefined-behavior gates can be enabled independently:

```sh
cmake -S . -B build-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLUNA_ENABLE_SANITIZERS=ON \
  -DLUNA_STRICT_WARNINGS=ON
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build-sanitized -LE hardware --output-on-failure
```

This instruments the compiler and in-process JIT Runtime with ASan/UBSan without
contaminating the installed `libruntime.a`; installed AOT tests need no additional
sanitizer-runtime link. ORC entry functions lack Clang UBSan function-type-prefix metadata,
so the single indirect call at a JIT entry disables `function` checking; compiler and
Runtime instrumentation before and after the call remains enabled. The Native artifact
verifier/loader test is also instrumented, including proof parsing, immutable staging,
typed-registry validation, and generation adaptation.

The concurrent generation state machine has a separate ThreadSanitizer gate so
TSan is never combined with ASan/UBSan or applied to LLVM/ORC:

```sh
cmake -S . -B build-thread-sanitized -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLUNA_ENABLE_THREAD_SANITIZER=ON -DLUNA_STRICT_WARNINGS=ON
cmake --build build-thread-sanitized --target moon-runtime-test
TSAN_OPTIONS=halt_on_error=1:history_size=7 \
  ctest --test-dir build-thread-sanitized \
    -R luna.moon-runtime --output-on-failure
```

This exercises four concurrent switchable readers across 1000 atomic generation
transitions and fails on the first reported data race.

Current `luna.semantic-regressions` covers:

- structured lexer/parser errors and top-level multi-error recovery; basic functions,
  arithmetic/bitwise/relational operators and logical short-circuiting; closures, generic
  monomorphization, `const`/`constexpr`, and compile-time reflection;
- valid nominal interceptor/context/slot paths, return/abort/resume cleanup, and explicit
  multi-shot/dynamic migration negatives;
- version selection for functions, types, and traits, plus incomplete implementations;
- multi-file packages, cross-file parse recovery, explicit exports, duplicate
  symbol/version identity, export/FFI boundaries, and package-name consistency;
- JIT/AOT C ABI `puts` linking, linear `free` transfer, and ABI, generic, and parameter
  type boundary negatives;
- path-sensitive ownership of linear resources: two-path consumption and one-path leaks in
  `if`, terminating early `return` paths, zero/multiple loop iterations, and unreachable
  statements;
- GPU in-flight buffers, unawaited events, and event-release requirements before early return;
- basic, versioned, and move-event heterogeneous computation on the CPU simulator;
- recursive/generic `Drop`, rejection of old `rc new`/`arc new` syntax, and active-payload
  cleanup for both Result variants;
- Result construction/discrimination/unwrapping, successful and error early returns through
  `?`, error-path cleanup, fragment-boundary rejection, and abort-style panic.

ROCm and CUDA hardware tests are not part of default CTest: default tests must run on CI hosts
without a GPU. ROCm smoke tests run additionally on hosts with an AMD GPU:

```sh
LUNA_GPU_BACKEND=rocm ./build/luna run examples/heterogeneous.luna \
  --gpu-target=rocm:gfx1101
```

Configuration may also enable `-DLUNA_ENABLE_ROCM_SMOKE=ON` and select the target ISA with
`-DLUNA_ROCM_SMOKE_ARCH=gfx1101`, followed by
`ctest --test-dir build -L rocm --output-on-failure`. This optional test separately checks
ROCm JIT and AOT kernel paths.

CPU comparison benchmarks can be enabled with `-DLUNA_ENABLE_CPU_BENCHMARK=ON`; see the
[benchmark guide](benchmarks.md).

Setting `-DLUNA_ENABLE_ROCM_BENCHMARK=ON` additionally registers
`luna.rocm-cpp23-comparison`. It compares Luna AOT with C++23/HIP over 16,777,216 elements
and ten transformation rounds, and verifies both results. It carries the `benchmark` label
and is outside the default test set.

Every semantic or diagnostic fix should add a minimal positive or negative example and assert
stable output or key diagnostic text in `tests/semantic_regressions.cmake`.

`luna.moonir-canonical` covers both forged table-level fixtures and a real frontend integration
path. Its default-fragment case runs source parsing and Sema through Luna lowering before building
and independently verifying the canonical CFG. It checks the construction-only implicit `Apply`,
an explicit static `apply`, context `Resume`, lexical LocalId capture, and preservation of reusable
fragment bodies. Legacy dynamic/multi-shot construction fixtures remain verifier corpus, while
their source spellings are rejected in the frontend. The function sealer
fixture additionally proves transactional replacement: a valid lowered function loses its
structured body and verifies with one CFG, simultaneous representations are rejected, and a
runtime-apply failure preserves the original module unchanged.
The same test then sends a sealed, cleanup-free function through the LLVM backend and ORC JIT.
Its nested scope intentionally shadows a local name; the result of 42 proves canonical storage is
keyed by `LocalId` while Jump/Branch/Return preserve source control semantics.
Another source-to-JIT module constructs and matches both an enum and `Result<i32, i32>`. It checks
that source `Ok` becomes a data-only canonical constructor, both matches become `Switch`, all three
payload bindings retain their pattern `LocalId`, and enum/Result ABI unpacking returns 42.
An additional generate-only module requires a root parameter cleanup on return and a lexical
cleanup on a branch edge, exercising ordered cleanup emission without executing a deliberately
synthetic owned-string fixture.

The canonical table fixtures also cover cursor-guarded cleanup for sequentially consumed
move-only arrays. Tamper cases reject mixed guarded/unguarded cleanup, duplicate element coverage,
and a guard that names a value local instead of its same-scope synthetic integer cursor.
The direct consuming-loop fixture additionally checks the atomic dynamic element transfer, cursor
reuse, exhaustion cleanup, early-return cleanup, a missing transfer witness, and an omitted unread
tail element.

`luna.rc-arc-core` uses the real Core package to verify JIT/AOT behavior, explicit trait/member
clone, implicit-copy rejection, exact-once last-handle release, nested payload Drop, ordinary
nominal MoonIR facts, and LLVM Runtime ABI v1 calls.

`luna.core-option` verifies the source-defined Core `Option<T>` extractors in
both JIT and AOT builds. It covers Copy values, affine `Some` payload transfer,
`None` fallback transfer, exact-once cleanup of an unselected fallback, and the
canonical `never`/panic branch becoming `Unreachable` rather than falling
through to a valueless return. Runtime-negative cases also check the stable
`unwrap(None)` message and caller-provided `expect(None)` message.

`luna.core-result` verifies the ordinary source-defined Core Result extraction
layer in JIT and AOT builds. It covers Copy values, affine Ok/Err payload
transfer, selected and unselected fallback cleanup, consumed error cleanup,
and the default or caller-provided panic messages for `unwrap`, `expect`,
`unwrap_err`, and `expect_err`.

`luna.package-export-abi` is an independent AOT ABI test: it verifies that exported
package functions are external symbols in LLVM IR while non-exported functions are
`internal`. The test removes its generated `.ll` and executable at the end.

`luna.return-cleanup-abi` tests path-sensitive release: every `return` in nested branches
and fall-through paths emits one `rt_dealloc` with exact `size/alignment` on its own path,
rather than leaving cleanup in an unreachable block tail.

`luna.result-error-aot` compares JIT/AOT output for Result propagation and checks that AOT
IR preserves `try.error`, resource cleanup, and the unwrap-panic boundary.

`luna.control-flow-aot` confirms that a conditional with both branches returning can
generate, link, and run valid AOT LLVM IR; semantic regression tests also reject uncovered
return paths in non-`unit` functions.

`luna.ffi-aot` builds and runs a C FFI example through the system linker, supplementing ABI
coverage beyond JIT process-symbol resolution.

`luna.jit-aot-parity` compares exit code and stdout for the same program under JIT and AOT,
including arithmetic/bitwise operations, relational comparisons, and short-circuit control flow.

`luna.optimization-pipeline` checks `-O0/-O2/-O3` entry points and compares `-O0` with
`-O2` IR: local stack slots should be promoted and constant computation folded. It also
checks the bounded four-way O3 hint on a straight-line reduction loop, excludes a tiny
nested recurrence from that hint, and requires optimized JIT/AOT to return the same result.

`luna.fragment-lowering-abi` checks that static fragments do not degrade into dynamic
candidate selection or heap allocation. `luna.structured-cps-abi` checks, at O0, the
canonical context continuation and runs a continuation-internal return case to ensure retained
fragment resources are cleaned and code after `resume()` is not executed.

`luna.legacy-dynamic-fragment-source` confirms that the removed dynamic fragment
source spelling fails at its migration boundary. There is no positive external
plugin ABI test because that production ABI was deleted in priority item 16.

`luna.aot-runtime-boundary` covers explicit `--runtime-lib`/`--cc`, the
`DRV0001` missing-runtime diagnostic, and GPU-backend initialization failure in an AOT
executable.

`luna.install-smoke` installs the current build into an isolated temporary prefix and
checks the driver, static Runtime, public ABI headers, standard-library workspace, and
semantic-reference documents. It then uses only the installation tree for `--version`,
`check`, JIT, and explicit Runtime/compiler AOT build-and-run checks. With `release` and
`install` labels, it is an automated gate for release-package layout.

`luna.compiler-identity` compares the source repository's current commit with the commit
reported by the compiler's structured analysis hello. The CMake project watches both Git
HEAD and the active branch ref, so an ordinary incremental build refreshes this identity
after a commit instead of silently publishing a stale build stamp.

`luna.ecosystem-frozen-baseline` verifies the exact, clean toolchain and Lunax commits recorded
as historical ecosystem evidence. `luna.ecosystem-release-policy` separately verifies the
fail-closed policy: a frozen snapshot may remain valid while publication stays blocked. The
prebuilt-release workflow reruns that policy with `REQUIRE_READY=ON`, which requires
`release.publish`, `status: release-ready`, and current-version Luna compatibility tags before
any platform packaging job starts.

`luna.runtime-gpu-error-state` verifies successful and invalid event ABI states on the CPU
simulator. `luna.gpu-error-boundary-abi` checks that an `await` failure branch in AOT IR
calls the unified GPU-error termination entry, so CUDA/ROCm launch or synchronization
failures cannot continue silently. The former also checks stable GPU domain/code fields in
the Runtime error snapshot, two-stage message copying, legacy `last_error` compatibility,
and that one operation error does not contaminate successful backend initialization.
`luna.runtime-abi-v1` covers host-service/error-snapshot behavior and C/C++
compilability of the public header.

`luna.jit-aot-extended-parity` compares, at `-O2`, JIT/AOT exit code and stdout for
multi-file packages and CPU-simulator heterogeneous programs, ensuring optimization does not
break package-level linking or host-side launch/event lowering.

`luna.stable-core-parity` is the full second-week consistency matrix. It separately compares
JIT/AOT stdout, exit code, and stderr for stable-core examples at `-O0`, `-O2`, and
`-O3`, covering generics, reflection, closures, ADTs, versioning, traits, FFI, multi-file
packages, and CPU-simulator heterogeneous programs. It also prevents ADT parameters from
being misclassified as heap memory owned by the callee, which would cause double release
after an ordinary call.
