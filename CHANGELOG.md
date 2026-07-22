# Changelog

## 0.2.0-alpha — Unreleased

- Added the versioned, C-compatible Runtime ABI v1 with replaceable host allocator and console services, exact `size/alignment` allocation lowering, foreign-resource carriers, and an optional W^X executable-memory capability reserved for MoonRuntime/JIT hosts.
- Moved compiler-generated `new`, automatic cleanup, explicit `free`, and language `print` behind Luna runtime symbols; kept layout-less `rt_malloc/rt_free` only as an Alpha compatibility bridge for previously emitted IR.
- Verified the stable JIT/AOT/runtime boundaries on Linux, macOS, and Windows UCRT64, including explicit ORC symbol registration and parameterized AOT process launching.
- Separated reverse-DNS Package IDs from `::` module/submodule identities, added `using <Package ID> as <alias>` dependency edges, and preserved the package/module graph in verified MoonIR.
- Added strict Alpha TOML schemas for `luna.package`, `luna.workspace`, and `luna.lock`, local workspace Package ID resolution, manifest source roots, a no-codegen `luna check` command, and logically independent `org.luna.core`/`org.luna.std` packages.

## 0.1.0-alpha — Development baseline

- Inserted verified MoonIR between the typed Luna frontend and both LLVM AOT/JIT paths.
- Replaced compiler-defined version tags with first-class Metadata, static/dynamic Selector operations, and runtime descriptors.
- Split Selector, Instantiator, PackageManager, and MacroProcessor into independent compiler components.
- Added audited cost reporting, exact generic instance IDs, reachable-only kernel emission, and `--reserve-kernel-runtime`.
- Split Value/Meta/Compiler type domains, made structs/enums structural by default with explicit `nominal` declarations, and added stable TypeId/ShapeId relations plus a verified MoonIR type table.
- Stabilized explicit package exports and C FFI; split ownership relation from Copy/Affine/Linear usage, added Place-based partial moves/borrows, and emitted verified MoonIR cleanup obligations.
- Added JIT/AOT parity coverage at `-O0`, `-O2`, and `-O3`, plus reproducible AOT runtime-library selection.
- Made JIT runtime resolution platform-independent with explicit ORC symbols, split explicit `--gpu-target` code-object generation from runtime-only `LUNA_GPU_BACKEND`, and replaced shell-based AOT linking with parameterized process execution.
- Added CPU simulator regression coverage, CUDA PTX/ROCm HSACO paths, observable GPU launch/event failures, bulk i32 transfer ABI, and optional ROCm JIT/AOT smoke testing.
- Added Linux CI, installation guidance, package documentation, Alpha limitations, and benchmark methodology.
- Added the Alpha v1 external fragment-plugin ABI for host-only single-shot interceptors, including contract validation and dynamic dispatch tests.
- Added release metadata, `luna --version`, installation staging checks, a root README, and a release checklist.
- Added dual MIT / Apache-2.0 licensing and documented the post-Alpha development roadmap.
- Added the Luna-PL project branding, portable LLVM CMake target discovery, and a lightweight CPU baseline benchmark.
