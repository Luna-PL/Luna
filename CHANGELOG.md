# Changelog

## 0.1.0-alpha — Unreleased

- Inserted verified MoonIR between the typed Luna frontend and both LLVM AOT/JIT paths.
- Replaced compiler-defined version tags with first-class Metadata, static/dynamic Selector operations, and runtime descriptors.
- Split Selector, Instantiator, PackageManager, and MacroProcessor into independent compiler components.
- Added audited cost reporting, exact generic instance IDs, reachable-only kernel emission, and `--reserve-kernel-runtime`.
- Split Value/Meta/Compiler type domains, made structs/enums structural by default with explicit `nominal` declarations, and added stable TypeId/ShapeId relations plus a verified MoonIR type table.
- Stabilized explicit package exports and C FFI; split ownership relation from Copy/Affine/Linear usage, added Place-based partial moves/borrows, and emitted verified MoonIR cleanup obligations.
- Added JIT/AOT parity coverage at `-O0`, `-O2`, and `-O3`, plus reproducible AOT runtime-library selection.
- Made JIT runtime resolution platform-independent with explicit ORC symbols, separated offline GPU code-object emission from device initialization, and replaced shell-based AOT linking with parameterized process execution.
- Added CPU simulator regression coverage, CUDA PTX/ROCm HSACO paths, observable GPU launch/event failures, bulk i32 transfer ABI, and optional ROCm JIT/AOT smoke testing.
- Added Linux CI, installation guidance, package documentation, Alpha limitations, and benchmark methodology.
- Added the Alpha v1 external fragment-plugin ABI for host-only single-shot interceptors, including contract validation and dynamic dispatch tests.
- Added release metadata, `luna --version`, installation staging checks, a root README, and a release checklist.
- Added dual MIT / Apache-2.0 licensing and documented the post-Alpha development roadmap.
- Added the Luna-PL project branding, portable LLVM CMake target discovery, and a lightweight CPU baseline benchmark.
