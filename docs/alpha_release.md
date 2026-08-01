# Luna 0.2.1 Prerelease Notes

> Document type: Release notes
> Applies to: Luna 0.2.1
> Status: Implemented Experimental
> Normativity: Partially normative
> Implementation checked: Pending confirmation on the 0.2.1 release commit (2026-08-01)

[简体中文版本](alpha_release.zh-CN.md)

## Position

Luna 0.2.1 is a prerelease compiler prototype for language experiments and internal
projects on Linux, macOS and Windows. The stable core is validated by native CI
on all three platforms; Windows uses the MSYS2 UCRT64 toolchain. The stable core
includes multi-file packages with explicit exports, verified MoonIR, basic
functions/generics/ADTs, linear ownership and borrowing, Runtime ABI v1,
explicit C FFI, JIT/AOT, `-O0/-O2/-O3`, and the initial heterogeneous ABI on the
CPU simulator.

The default regression suite covers semantic positives and negatives, packages,
export ABI, FFI, control-flow cleanup, JIT/AOT parity, optimization, runtime
boundaries and the CPU GPU simulator. ROCm hardware smoke tests are optional;
see [heterogeneous compute](heterogeneous_compute.md).

## Long-term maintenance status

`0.2.1` is the current long-term maintenance line, not a short transition
label leading to a near-term Beta. There is no scheduled Beta, `0.3`, or language
version upgrade. The public version string remains `0.2.1`; different
builds and toolchain snapshots are identified by source commit, release manifest
and checksum. The version string alone does not prove that two binaries are
identical.

Tagged prerelease artifacts are immutable. The release workflow will not replace
an existing `v0.2.1` Release. Later same-version development snapshots must
carry a commit identity and must not silently replace published archives.

Near-term development focuses on the toolchain:

- diagnostic quality, source locations and error explanations;
- formatter, language-server and editor integration;
- build, test, package, workspace and local dependency workflows;
- installation, prebuilt distribution, reproducible builds and portability;
- benchmark, profiling, debugging and developer-audit tools.

Expanding the syntax or type surface is not a near-term language goal. Correctness
and safety defects, deviations from frozen contracts, missing negative tests and
internal interfaces blocking the toolchain may still be addressed. New language
capabilities require a separate version-impact decision and may not enter the
stable core implicitly through toolchain work.

## Known limitations

- Explicit `interceptor` / `context` / `slot`, `constexpr` / reflection and
  complex trait-version combinations remain experimental semantics.
- The external fragment plugin provides the Alpha v1 ABI but supports only
  host-only, single-shot, explicitly parameterized `interceptor` fragments.
  `context`, `resume()`, multi-shot dispatch and lexical captures cannot cross a
  shared-library boundary.
- Runtime ABI v1 fixes allocator, console, foreign-resource and optional
  executable-memory host boundaries. Moon container loading, verification and
  hotspot/JIT policy remain future MoonRuntime work.
- Safe fixed-size `array<T, N>`, read-only borrowed `slice<T>`, array literals and
  bounds-checked indexing are available. Mutable slices and owning heap
  containers are not. The GPU data model is centered on `device_buffer<i32>`, a
  one-dimensional grid and fixed 256-thread blocks; host bulk copies use low-level
  `raw<i32>` pointers.
- The large ROCm comparison runs a 64 MiB, ten-round workload against C++23/HIP.
  `LUNA_GPU_PROFILE=1` prints accumulated CUDA/ROCm device-event kernel time;
  this is a runtime profiling switch, not a stable source-language timing API.
- CUDA code generation exists but is not validated by NVIDIA hardware CI in this
  repository.
- ROCm smoke tests require an AMD GPU, HIP runtime, kernel driver and matching
  `--gpu-target=rocm:gfx*`. Basic JIT/AOT smoke tests passed on an RX 7800 XT;
  broader hardware coverage, long-run stability and performance data remain open.
- Packages support strict `luna.package`, local `luna.workspace`, `luna.lock`,
  exact versions and recursive workspace loading. Remote registries, content
  digest verification, caching and network dependency resolution are not implemented.
- Installed AOT builds must receive `--runtime-lib` / `--cc`, or
  `LUNA_RUNTIME_LIB` / `LUNA_CXX` must be set.
- The REPL supports only `= <i32 expression>`, `:decl <complete one-line
  declaration>`, one-line temporary statements and `:help/:reset/:quit`.
  Declarations persist through source recompilation; locals, heap values, JIT
  globals and runtime state do not persist, and multiline input is unsupported.

## Prebuilt packages

The `v0.2.1` tag triggers the release workflow. The workflow uploads the
following archives with SHA-256 files only after the platform's complete
non-hardware CTest suite passes:

| Archive | Support scope |
|---|---|
| `luna-0.2.1-linux-x86_64.tar.gz` | Ubuntu 24.04, x86_64, glibc |
| `luna-0.2.1-macos-<arch>.tar.gz` | macOS 14; `<arch>` is the runner architecture |
| `luna-0.2.1-windows-ucrt64-x86_64.zip` | Windows x86_64, MSYS2 UCRT64 |

Archives contain `luna`, the static Luna Runtime, public Runtime ABI headers,
the standard library, documentation and LLVM dynamic libraries required by the
compiler (the Windows package includes its UCRT64 DLL closure). Each archive
root contains `RELEASE-MANIFEST.txt` with its target, dependencies and source
commit.

These are prebuilt compiler distributions, not fully self-contained native SDKs:

- `luna check`, CPU JIT and the simulator backend work with the packaged compiler;
- `luna build` still requires a compatible platform LLVM 22 `clang++` and should
  pass `--runtime-lib <unpacked>/lib/libruntime.a` and `--cc <clang++>`;
- CUDA/ROCm still require host drivers, user-space runtimes and matching hardware;
- distributions outside the listed targets, old macOS versions, the MSVC ABI and
  the MSYS2 MSVCRT environment are unsupported.

## Upgrade commitment

Alpha error codes and safety semantics should remain compatible; experimental
features may still change. Leaving the `0.2.1` long-term line requires a
separate version decision, semantic-baseline review, migration notes and the full
release gate. It is not triggered automatically by elapsed time or toolchain
completion. Every semantic change must add positive and negative examples plus
migration guidance.

## Release checklist

- [x] Version metadata and `luna --version` agree on `0.2.1`.
- [x] Native Linux C++17/C++23, macOS and Windows UCRT64 CI cover the stable core.
- [x] Linux-owned targets use strict warnings and full ASan/UBSan non-hardware regression.
- [x] Non-hardware CTest covers semantics, packages, FFI, ownership, CPS, optimization and plugin ABI.
- [x] The install tree contains the driver, Runtime, public ABI headers, standard-library workspace and docs.
- [x] `luna.install-smoke` checks version, `check`, JIT and AOT from an isolated install tree.
- [x] Build outputs are ignored and historical `build-cpp23` artifacts were removed.
- [x] MIT / Apache-2.0 licenses ship with installation results.
- [x] The release workflow builds, tests, packages and checksums all three targets.
- [ ] The three packaging jobs and publish job pass for the `v0.2.1` tag.
- [ ] The three prerelease archives and checksum files pass download verification.

The release host must repeat the install-tree JIT/AOT checks. AOT must pass a
matching `--runtime-lib` and `--cc`, or set the corresponding environment variables.
ROCm release validation requires a visible device, driver and matching ISA; CUDA
still lacks NVIDIA hardware CI in this repository. No-GPU hosts do not turn a
hardware-only failure into a stable-core regression.

The Alpha release does not include remote registries/network dependencies,
external `context`/`resume`, multi-shot plugins, general owning heap containers or
a stable source-language profiling API.
