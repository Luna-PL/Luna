# Luna Repository File and Responsibility Guide

> Document category: implementation note / project policy
> Applies to: Luna 0.2.1
> Status: Internal
> Normative status: normative
> Implementation audit: pending this change (2026-07-31)

This is the authoritative index of physical repository directories and file responsibilities.
It answers where code and documentation belong without redefining language semantics. Language
contracts remain defined by the 0.2 Alpha semantic baseline and its reference documents.

## 1. Usage rules

1. Every repository file must appear in the precise inventory at the end and inherits the
   nearest directory rule; a file-specific entry takes precedence.
2. Each file should have one primary responsibility. Orchestration spanning multiple stages
   belongs in a dedicated orchestrator rather than an existing implementation file.
3. Headers declare boundaries and stable data shapes; source files implement those boundaries.
   Header logic is allowed only when cross-translation-unit reuse is genuinely useful.
4. src must not depend on tests, examples, or benchmarks. core must not depend on driver,
   parser, sema, MoonIR, or codegen; backends must not call semantic analysis in reverse.
5. Test scripts assert behavior and fixtures provide inputs. Examples teach and are not the sole
   regression evidence.
6. Normative facts have one source of truth; READMEs, architecture diagrams, and topic guides
   summarize and link to it.
7. Adding, moving, or deleting a file requires updating this guide. luna.file-guide-inventory
   checks the precise inventory.
8. Human-facing Markdown uses English filenames and default content; Chinese uses a matching
   .zh-CN.md companion. Source, fixtures, lockfiles, licenses, and machine configuration are
   not duplicated as translations.

## 2. Dependency direction

~~~text
main -> driver
driver -> package/parser -> sema -> MoonIR -> codegen
                         \-> selector / instantiation
codegen -> runtime ABI

core / diagnostics are consumed by upper layers only
stdlib, examples, benchmarks, and tests consume the compiler
~~~

Cross-layer declarations are allowed for data models, but not reverse-stage execution. MoonIR
may retain type identity but must not query SemanticAnalyzer again; codegen may consume cleanup
obligations but must not rederive ownership.

## 3. Directory responsibilities

| Directory | Primary responsibility | Must not contain |
|---|---|---|
| .github/workflows/ | CI, Sanitizer, cross-platform build, release automation | Language semantics or CI-only semantic fixes |
| benchmarks/ | Reproducible performance workloads and runners | Correctness gates or unexplained performance claims |
| docs/ | User docs, implementation notes, roadmap, release boundaries | Duplicate authoritative language rules |
| docs/reference/ | Alpha contracts, semantic baseline, type and error models | Tutorials, roadmap, temporary status |
| examples/ | Independent readable language examples | Sole regression evidence or test assertions |
| examples/full_showcase/ | Complete workspace/package integration example | Compiler-internal test logic |
| examples/slot_plugins/ | Slot/fragment plugin examples | Runtime ABI authority |
| src/ | Compiler and embedded Runtime implementation | Test data or tutorials |
| src/core/ | Backend-independent type, identity, layout, ownership, sysmeta models | AST, symbol tables, LLVM |
| src/diagnostics/ | Cross-stage diagnostic format | Stage-specific semantic decisions |
| src/driver/ | CLI, pipeline orchestration, REPL, JIT/AOT shell | Lexer, semantic, or codegen rules |
| src/lexer/ | Token definitions and lexing | AST or type checking |
| src/parser/ | AST data and parsing | Type inference or LLVM lowering |
| src/sema/ | Names, types, traits, ownership, borrow checking | MoonIR optimization or machine ABI |
| src/selector/ | Independent candidate-selection model | Parser or codegen orchestration |
| src/instantiation/ | Generic-instance requests, state, stable identity | AST semantic cloning or LLVM emission |
| src/moonir/ | Backend IR, lowering, verification, optimization, printing | Source name resolution or target details |
| src/codegen/ | Verified MoonIR to LLVM/JIT/AOT lowering | Re-running semantic or ownership inference |
| src/runtime/ | Runtime ABI, host services, GPU backend, plugin boundaries | Compile-time semantics |
| src/package/ | Package/module/workspace/lock loading | Unimplemented remote registry behavior |
| stdlib/ | Installed Luna core/standard-library workspace | Duplicated compiler hardcoding |
| tests/ | CTest drivers, ABI tests, assertions | User tutorials |
| tests/fixtures/ | Positive/negative/package/workspace inputs | CMake assertions or orchestration |
| tools/ | Developer helper commands | An implicit CI-only required step |

Unlisted future files under src subdirectories inherit their directory rule. A fixture ending
in _invalid is expected to be rejected; other Luna fixtures are positive, runtime-failure, or
script-defined special inputs.

## 4. Root files and automation

- CMakeLists.txt owns targets, installation layout, and CTest registration; assertions remain in
  tests scripts.
- VERSION is the sole complete release-version source.
- CHANGELOG.md records user-visible changes and migration; it does not replace references.
- README.md is the English project entry; README.zh-CN.md is its Chinese counterpart.
- CI workflows own platform gates; release.yml owns tag checks, prebuilt packages, checksums,
  and prerelease publication. release-evidence.yml independently checks locked public release
  metadata and asset digests.

## 5. Source responsibility boundaries

- Driver files own command dispatch, pipeline orchestration, REPL, and AOT linking.
- Package files own manifests, locks, source graphs, and package merging.
- Core files own ownership, sysmeta, type identity, type relations, and layout.
- Lexer/parser files own tokens, AST, parsing, and syntax recovery.
- Sema files own names, types, generics, constexpr, traits, ownership, borrowing, and typed AST.
- Tooling files own versioned in-memory source documents, editor position
  conversion, and retained read-only frontend analysis snapshots.
- Selector and instantiation files own candidate selection and generic-instance state.
- MoonIR files own lowering, verification, optimization, and deterministic printing.
- Codegen files are split by module, function, statement, expression, cleanup, execution,
  fragments, GPU, iterators, range analysis, descriptors, and helper concerns.
- Runtime files own versioned C ABIs and host/GPU/plugin services.

SemanticAnalyzer remains a large stage implementation. New semantic subsystems must establish a
narrow interface before splitting translation units; splitting only to reduce line count is not
sufficient.

## 6. Documentation, examples, and tests

English is the default documentation entry point. Every human-facing document should have a
matching Chinese companion, with the exception of source code, fixtures, lockfiles, licenses,
and machine configuration. The former getting_started.en.md is retained only as a short
migration entry; the canonical English file is getting_started.md and the Chinese file is
getting_started.zh-CN.md.

The documentation migration is complete for the current reference, architecture, roadmap,
testing, runtime ABI, package, iterator, standard-library, compile-time, fragment,
heterogeneous-compute, and versioning documents. Only the transitional getting-started entry
remains outside the canonical pair.

The paired Luna 0.3 evolution audit records the current model-convergence and
slot/fragment refactoring assessment. It is a design audit, not part of the
0.2 normative reference or the active implementation roadmap.

Examples demonstrate one topic per file; full_showcase is the combined Alpha example and
slot_plugins demonstrates plugin use. Benchmark runners are responsible for reproducible
build, sampling, and reporting. `cpp23_allocation_support.cpp` keeps the C++ allocation
workload observable across a non-LTO translation-unit boundary.

Test scripts own assertions for exit status, diagnostics, IR, ABI, and output. Fixtures own
source inputs. semantic_regressions.cmake is the positive/negative language matrix; other
scripts cover AOT/JIT, MoonIR, packages, Runtime, GPU, fragments, iterators, install, and
release boundaries.

## 7. Exact file inventory

This inventory is checked by luna.file-guide-inventory. Each entry inherits the nearest rule
above; the inventory proves coverage and does not repeat semantic explanations. Build trees,
Git internals, and ignored generated artifacts are excluded.

<!-- FILE_INVENTORY_BEGIN -->
- `.github/workflows/linux-ci.yml`
- `.github/workflows/macos-ci.yml`
- `.github/workflows/release-evidence.yml`
- `.github/workflows/release.yml`
- `.github/workflows/windows-ci.yml`
- `.gitignore`
- `CHANGELOG.md`
- `CMakeLists.txt`
- `LICENSE-APACHE`
- `LICENSE-MIT`
- `README.md`
- `README.zh-CN.md`
- `VERSION`
- `ecosystem.lock.json`
- `benchmarks/cpp23_allocation_support.cpp`
- `benchmarks/cpp23_cpu_suite.cpp`
- `benchmarks/cpp23_hip_vector.cpp`
- `benchmarks/luna_cpu_allocation.luna`
- `benchmarks/luna_cpu_arithmetic.luna`
- `benchmarks/luna_cpu_array.luna`
- `benchmarks/luna_cpu_array_scan.luna`
- `benchmarks/luna_cpu_bitmix.luna`
- `benchmarks/luna_cpu_branch.luna`
- `benchmarks/luna_cpu_calls.luna`
- `benchmarks/luna_cpu_nested.luna`
- `benchmarks/luna_cpu_reduction.luna`
- `benchmarks/luna_gpu_vector.luna`
- `benchmarks/run_basic_benchmark.sh`
- `benchmarks/run_cpu_comparison.sh`
- `benchmarks/run_rocm_cpp23_comparison.sh`
- `docs/alpha_release.md`
- `docs/alpha_release.zh-CN.md`
- `docs/architecture.md`
- `docs/architecture.zh-CN.md`
- `docs/benchmarks.md`
- `docs/benchmarks.zh-CN.md`
- `docs/cli.md`
- `docs/cli.zh-CN.md`
- `docs/compile_time.md`
- `docs/compile_time.zh-CN.md`
- `docs/decisions.md`
- `docs/decisions.zh-CN.md`
- `docs/ecosystem_release.md`
- `docs/ecosystem_release.zh-CN.md`
- `docs/features.md`
- `docs/features.zh-CN.md`
- `docs/file_guide.md`
- `docs/file_guide.zh-CN.md`
- `docs/fragments.md`
- `docs/fragments.zh-CN.md`
- `docs/getting_started.en.md`
- `docs/getting_started.md`
- `docs/getting_started.zh-CN.md`
- `docs/heterogeneous_compute.md`
- `docs/heterogeneous_compute.zh-CN.md`
- `docs/iterators.md`
- `docs/iterators.zh-CN.md`
- `docs/luna_0.3_evolution_audit.md`
- `docs/luna_0.3_evolution_audit.zh-CN.md`
- `docs/packages.md`
- `docs/packages.zh-CN.md`
- `docs/reference/README.md`
- `docs/reference/README.zh-CN.md`
- `docs/reference/builtin_types.md`
- `docs/reference/builtin_types.zh-CN.md`
- `docs/reference/documentation_rules.md`
- `docs/reference/documentation_rules.zh-CN.md`
- `docs/reference/error_model.md`
- `docs/reference/error_model.zh-CN.md`
- `docs/reference/semantic_baseline_0.2.md`
- `docs/reference/semantic_baseline_0.2.zh-CN.md`
- `docs/reference/type_system.md`
- `docs/reference/type_system.zh-CN.md`
- `docs/roadmap.md`
- `docs/roadmap.zh-CN.md`
- `docs/runtime_abi.md`
- `docs/runtime_abi.zh-CN.md`
- `docs/standard_library.md`
- `docs/standard_library.zh-CN.md`
- `docs/testing.md`
- `docs/testing.zh-CN.md`
- `docs/versioning.md`
- `docs/versioning.zh-CN.md`
- `docs/windows_build.md`
- `docs/windows_build.zh-CN.md`
- `examples/.gitignore`
- `examples/adt.luna`
- `examples/adt_error.luna`
- `examples/basic.luna`
- `examples/closure.luna`
- `examples/compile_time.luna`
- `examples/dynamic_select.luna`
- `examples/ffi.luna`
- `examples/fragment_multishot_free_invalid.luna`
- `examples/fragment_multishot_invalid.luna`
- `examples/fragments.luna`
- `examples/full_showcase/README.md`
- `examples/full_showcase/app/luna.package`
- `examples/full_showcase/app/src/foreign.luna`
- `examples/full_showcase/app/src/main.luna`
- `examples/full_showcase/foundation/luna.package`
- `examples/full_showcase/foundation/src/algorithms.luna`
- `examples/full_showcase/foundation/src/device.luna`
- `examples/full_showcase/foundation/src/dispatch.luna`
- `examples/full_showcase/foundation/src/effects.luna`
- `examples/full_showcase/foundation/src/model.luna`
- `examples/full_showcase/luna.lock`
- `examples/full_showcase/luna.workspace`
- `examples/generic.luna`
- `examples/heap.luna`
- `examples/heterogeneous`
- `examples/heterogeneous.luna`
- `examples/heterogeneous_inflight_invalid.luna`
- `examples/heterogeneous_move_event.luna`
- `examples/heterogeneous_unawaited_invalid.luna`
- `examples/heterogeneous_versioned.luna`
- `examples/inference.luna`
- `examples/inference_error.luna`
- `examples/meta_select.luna`
- `examples/minimal.luna`
- `examples/operators.luna`
- `examples/print.luna`
- `examples/slot_plugins/README.md`
- `examples/slot_plugins/README.zh-CN.md`
- `examples/slot_plugins/loop_plugins.luna`
- `examples/test.luna`
- `examples/test2.luna`
- `examples/trait_versioned_nominal.luna`
- `examples/trait_versioning.luna`
- `examples/trait_versioning_incomplete_invalid.luna`
- `examples/trait_versioning_invalid.luna`
- `examples/versioning.luna`
- `examples/versioning_invalid.luna`
- `src/Version.h`
- `src/codegen/CGHelpers.cpp`
- `src/codegen/CGHelpers.h`
- `src/codegen/CodeGenerator.cpp`
- `src/codegen/CodeGenerator.h`
- `src/codegen/CodeGeneratorCleanup.cpp`
- `src/codegen/CodeGeneratorExecution.cpp`
- `src/codegen/CodeGeneratorExpressions.cpp`
- `src/codegen/CodeGeneratorFragments.cpp`
- `src/codegen/CodeGeneratorFunctions.cpp`
- `src/codegen/CodeGeneratorGpu.cpp`
- `src/codegen/CodeGeneratorIterator.cpp`
- `src/codegen/CodeGeneratorModule.cpp`
- `src/codegen/CodeGeneratorRangeAnalysis.cpp`
- `src/codegen/CodeGeneratorRangeAnalysis.h`
- `src/codegen/CodeGeneratorRuntimeDescriptors.cpp`
- `src/codegen/CodeGeneratorStatements.cpp`
- `src/core/CoreContracts.h`
- `src/core/Ownership.h`
- `src/core/SysMeta.h`
- `src/core/TypeIdentity.h`
- `src/core/TypeLayout.cpp`
- `src/core/TypeLayout.h`
- `src/core/TypeRelations.cpp`
- `src/core/TypeRelations.h`
- `src/core/TypeSystem.h`
- `src/diagnostics/Diagnostic.h`
- `src/driver/AotLinker.cpp`
- `src/driver/AotLinker.h`
- `src/driver/CommandLine.cpp`
- `src/driver/CommandLine.h`
- `src/driver/CompilerPipeline.cpp`
- `src/driver/CompilerPipeline.h`
- `src/driver/Driver.cpp`
- `src/driver/Driver.h`
- `src/driver/Repl.cpp`
- `src/driver/Repl.h`
- `src/instantiation/Instantiator.cpp`
- `src/instantiation/Instantiator.h`
- `src/lexer/Lexer.cpp`
- `src/lexer/Lexer.h`
- `src/lexer/Token.h`
- `src/macro/MacroProcessor.cpp`
- `src/macro/MacroProcessor.h`
- `src/main.cpp`
- `src/moonir/Lowering.cpp`
- `src/moonir/Lowering.h`
- `src/moonir/MoonIR.cpp`
- `src/moonir/MoonIR.h`
- `src/moonir/Optimizer.cpp`
- `src/moonir/Optimizer.h`
- `src/moonir/Printer.cpp`
- `src/moonir/Printer.h`
- `src/moonir/Verifier.cpp`
- `src/moonir/Verifier.h`
- `src/package/Package.cpp`
- `src/package/Package.h`
- `src/package/PackageManager.cpp`
- `src/package/PackageManager.h`
- `src/parser/AST.h`
- `src/parser/Parser.cpp`
- `src/parser/Parser.h`
- `src/runtime/FragmentPluginABI.h`
- `src/runtime/ApplicationHostServices.cpp`
- `src/runtime/ApplicationHostServices.h`
- `src/runtime/Runtime.cpp`
- `src/runtime/Runtime.h`
- `src/runtime/RuntimeABI.h`
- `src/selector/Selector.cpp`
- `src/selector/Selector.h`
- `src/sema/Inference.h`
- `src/sema/OwnershipChecker.cpp`
- `src/sema/OwnershipChecker.h`
- `src/sema/SemanticAnalyzer.cpp`
- `src/sema/SemanticAnalyzer.h`
- `src/sema/SymbolTable.cpp`
- `src/sema/SymbolTable.h`
- `src/sema/TraitChecker.cpp`
- `src/sema/TraitChecker.h`
- `src/sema/TypeSystem.cpp`
- `src/sema/TypeSystem.h`
- `src/tooling/SourceManager.cpp`
- `src/tooling/SourceManager.h`
- `src/tooling/AnalysisSnapshot.cpp`
- `src/tooling/AnalysisSnapshot.h`
- `src/tooling/ReferenceIndex.cpp`
- `src/tooling/ReferenceIndex.h`
- `src/tooling/SymbolIndex.cpp`
- `src/tooling/SymbolIndex.h`
- `stdlib/core/luna.package`
- `stdlib/core/src/error.luna`
- `stdlib/core/src/iter.luna`
- `stdlib/core/src/option.luna`
- `stdlib/core/src/prelude.luna`
- `stdlib/sys/luna.package`
- `stdlib/sys/src/alloc.luna`
- `stdlib/sys/src/console.luna`
- `stdlib/sys/src/fs.luna`
- `stdlib/sys/src/prelude.luna`
- `stdlib/alloc/luna.package`
- `stdlib/alloc/src/prelude.luna`
- `stdlib/luna.lock`
- `stdlib/luna.workspace`
- `stdlib/std/luna.package`
- `stdlib/std/src/io.luna`
- `tests/aot_runtime_boundary.cmake`
- `tests/analysis_protocol.cmake`
- `tests/analysis_snapshot_test.cpp`
- `tests/compiler_identity.cmake`
- `tests/control_flow_aot.cmake`
- `tests/core_contracts_test.cpp`
- `tests/core_surface.cmake`
- `tests/diagnostic_protocol.cmake`
- `tests/external_fragment_dispatch.cmake`
- `tests/ffi_aot.cmake`
- `tests/file_guide_inventory.cmake`
- `tests/fixtures/aot_runtime_boundary.luna`
- `tests/fixtures/apply_contract_checked_eagerly_invalid.luna`
- `tests/fixtures/array_move_element_invalid.luna`
- `tests/fixtures/comparison_non_numeric_invalid.luna`
- `tests/fixtures/comparison_operators.luna`
- `tests/fixtures/concept_not_satisfied_invalid.luna`
- `tests/fixtures/concepts.luna`
- `tests/fixtures/constexpr_nonconstant.luna`
- `tests/fixtures/context_abort_after_resume_invalid.luna`
- `tests/fixtures/context_abort_leaks_local_invalid.luna`
- `tests/fixtures/context_abort_ownership_mismatch_invalid.luna`
- `tests/fixtures/context_abort_preserves_outer_resource.luna`
- `tests/fixtures/context_continuation_return_invalid.luna`
- `tests/fixtures/context_continuation_return_valid.luna`
- `tests/fixtures/context_linear_guard.luna`
- `tests/fixtures/context_missing_control_invalid.luna`
- `tests/fixtures/context_partial_resume_invalid.luna`
- `tests/fixtures/context_return_ends_fragment_valid.luna`
- `tests/fixtures/core_surface_app/luna.package`
- `tests/fixtures/core_surface_app/src/main.luna`
- `tests/fixtures/drop_intrinsic.luna`
- `tests/fixtures/dynamic_apply_static_slot_invalid.luna`
- `tests/fixtures/dynamic_candidate_contract_mismatch_invalid.luna`
- `tests/fixtures/dynamic_fragments.luna`
- `tests/fixtures/enum_match.luna`
- `tests/fixtures/enum_match_arity_invalid.luna`
- `tests/fixtures/enum_match_duplicate_invalid.luna`
- `tests/fixtures/enum_match_non_exhaustive_invalid.luna`
- `tests/fixtures/enum_match_resource.luna`
- `tests/fixtures/external_fragment_dispatch.luna`
- `tests/fixtures/ffi_generic_invalid.luna`
- `tests/fixtures/ffi_owning_return.luna`
- `tests/fixtures/ffi_owning_return_ignored_invalid.luna`
- `tests/fixtures/ffi_owning_return_type_invalid.luna`
- `tests/fixtures/ffi_result_boundary_invalid.luna`
- `tests/fixtures/ffi_unsupported_abi_invalid.luna`
- `tests/fixtures/ffi_unsupported_type_invalid.luna`
- `tests/fixtures/fragment_contracts.luna`
- `tests/fixtures/generic_argument_count_invalid.luna`
- `tests/fixtures/generic_body_cloning.luna`
- `tests/fixtures/generic_instance_reuse.luna`
- `tests/fixtures/heterogeneous_bulk_transfer.luna`
- `tests/fixtures/heterogeneous_bulk_transfer_invalid.luna`
- `tests/fixtures/interceptor_resume_invalid.luna`
- `tests/fixtures/invalid_export.luna`
- `tests/fixtures/iterator_count_move_only_invalid.luna`
- `tests/fixtures/iterator_filter_move_only_owning_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_borrow_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_ignored_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_linear_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_move_invalid.luna`
- `tests/fixtures/iterator_fold_move_only_invalid.luna`
- `tests/fixtures/iterator_for_each_move_only_invalid.luna`
- `tests/fixtures/iterator_map_move_only_input_invalid.luna`
- `tests/fixtures/iterator_map_type_invalid.luna`
- `tests/fixtures/iterator_materialized.luna`
- `tests/fixtures/iterator_materialized_borrow_invalid.luna`
- `tests/fixtures/iterator_materialized_linear_source_invalid.luna`
- `tests/fixtures/iterator_materialized_move_only.luna`
- `tests/fixtures/iterator_materialized_source_use_after_invalid.luna`
- `tests/fixtures/iterator_materialized_twice_invalid.luna`
- `tests/fixtures/iterator_move_only_array.luna`
- `tests/fixtures/iterator_move_only_array_use_after_invalid.luna`
- `tests/fixtures/iterator_mut_borrow_conflict_invalid.luna`
- `tests/fixtures/iterator_pipeline.luna`
- `tests/fixtures/iterator_slice.luna`
- `tests/fixtures/iterator_terminal_use_after_invalid.luna`
- `tests/fixtures/jit_aot_parity.luna`
- `tests/fixtures/kernel_host_effects_invalid.luna`
- `tests/fixtures/kernel_unused.luna`
- `tests/fixtures/lambda_capture_invalid.luna`
- `tests/fixtures/lambda_linear_parameter_invalid.luna`
- `tests/fixtures/legacy_fragment_invalid.luna`
- `tests/fixtures/logical_short_circuit.luna`
- `tests/fixtures/missing_return_invalid.luna`
- `tests/fixtures/never_return_value_invalid.luna`
- `tests/fixtures/never_type.luna`
- `tests/fixtures/nominal_modifier_invalid.luna`
- `tests/fixtures/optimization_constant_fold.luna`
- `tests/fixtures/ownership_affine_drop.luna`
- `tests/fixtures/ownership_affine_requires_move_invalid.luna`
- `tests/fixtures/ownership_all_return_paths_consume.luna`
- `tests/fixtures/ownership_disjoint_field_borrows.luna`
- `tests/fixtures/ownership_heap_parameter_borrow.luna`
- `tests/fixtures/ownership_if_all_paths_consume.luna`
- `tests/fixtures/ownership_if_partial_consume_invalid.luna`
- `tests/fixtures/ownership_loop_consumes_outer_invalid.luna`
- `tests/fixtures/ownership_loop_local_resource_valid.luna`
- `tests/fixtures/ownership_overlapping_field_borrows_invalid.luna`
- `tests/fixtures/ownership_partial_move_branch_invalid.luna`
- `tests/fixtures/ownership_partial_move_invalid.luna`
- `tests/fixtures/ownership_return_cleanup.luna`
- `tests/fixtures/ownership_return_path_leaks_invalid.luna`
- `tests/fixtures/ownership_return_path_valid.luna`
- `tests/fixtures/ownership_unreachable_after_return.luna`
- `tests/fixtures/package_using_missing_alias_invalid.luna`
- `tests/fixtures/packages/alias_collision/01_first.luna`
- `tests/fixtures/packages/alias_collision/02_second.luna`
- `tests/fixtures/packages/duplicate_export/01_shared.luna`
- `tests/fixtures/packages/duplicate_export/02_shared.luna`
- `tests/fixtures/packages/duplicate_export/03_main.luna`
- `tests/fixtures/packages/duplicate_version/01_greet.luna`
- `tests/fixtures/packages/duplicate_version/02_greet.luna`
- `tests/fixtures/packages/duplicate_version/03_main.luna`
- `tests/fixtures/packages/exported_package/01_math.luna`
- `tests/fixtures/packages/exported_package/02_main.luna`
- `tests/fixtures/packages/mismatched_package/01_first.luna`
- `tests/fixtures/packages/mismatched_package/02_second.luna`
- `tests/fixtures/packages/module_headers/01_math.luna`
- `tests/fixtures/packages/module_headers/02_main.luna`
- `tests/fixtures/packages/method_references/01_ops.luna`
- `tests/fixtures/packages/method_references/02_main.luna`
- `tests/fixtures/packages/member_references/01_models.luna`
- `tests/fixtures/packages/member_references/02_main.luna`
- `tests/fixtures/packages/multiple_parse_errors/01_first.luna`
- `tests/fixtures/packages/multiple_parse_errors/02_second.luna`
- `tests/fixtures/packages/self_using/01_main.luna`
- `tests/fixtures/panic.luna`
- `tests/fixtures/panic_message_type_invalid.luna`
- `tests/fixtures/print_unsupported_type_invalid.luna`
- `tests/fixtures/parse_missing_binding_name.luna`
- `tests/fixtures/parse_multiple_declarations_invalid.luna`
- `tests/fixtures/rc_arc.luna`
- `tests/fixtures/rc_implicit_copy_invalid.luna`
- `tests/fixtures/recursive_structural_type_invalid.luna`
- `tests/fixtures/reflection_index_out_of_range.luna`
- `tests/fixtures/repl_session.txt`
- `tests/fixtures/result_ambiguous_constructor_invalid.luna`
- `tests/fixtures/result_basic.luna`
- `tests/fixtures/result_from_borrowed_source_invalid.luna`
- `tests/fixtures/result_from_conversion.luna`
- `tests/fixtures/result_from_resource.luna`
- `tests/fixtures/result_from_signature_invalid.luna`
- `tests/fixtures/result_match.luna`
- `tests/fixtures/result_match_non_exhaustive_invalid.luna`
- `tests/fixtures/result_match_resource.luna`
- `tests/fixtures/result_payload_abi_invalid.luna`
- `tests/fixtures/result_propagation.luna`
- `tests/fixtures/result_resource_cleanup.luna`
- `tests/fixtures/result_try_error_mismatch_invalid.luna`
- `tests/fixtures/result_try_fragment_invalid.luna`
- `tests/fixtures/result_try_non_result_function_invalid.luna`
- `tests/fixtures/result_try_non_result_invalid.luna`
- `tests/fixtures/result_unwrap_panic.luna`
- `tests/fixtures/safe_array_static_bounds_invalid.luna`
- `tests/fixtures/safe_array_wrong_element_invalid.luna`
- `tests/fixtures/safe_arrays.luna`
- `tests/fixtures/selector_outside_view_invalid.luna`
- `tests/fixtures/selector_user_logic.luna`
- `tests/fixtures/slice_borrow.luna`
- `tests/fixtures/slice_bounds_invalid.luna`
- `tests/fixtures/slice_empty_tail.luna`
- `tests/fixtures/slice_write_source_invalid.luna`
- `tests/fixtures/slot_cardinality_contract_mismatch_invalid.luna`
- `tests/fixtures/slot_fragment_contract_mismatch_invalid.luna`
- `tests/fixtures/slot_missing_contract_invalid.luna`
- `tests/fixtures/static_declaration_reflection.luna`
- `tests/fixtures/structural_enum_equivalence.luna`
- `tests/fixtures/structural_field_order_invalid.luna`
- `tests/fixtures/structural_generic_instance_reuse.luna`
- `tests/fixtures/structural_trait_coherence.luna`
- `tests/fixtures/structural_type_equivalence.luna`
- `tests/fixtures/type_domains_reflection.luna`
- `tests/fixtures/type_relations.luna`
- `tests/fixtures/versioned_fragment_contract_change_invalid.luna`
- `tests/fixtures/workspaces/local/app/luna.package`
- `tests/fixtures/workspaces/local/app/src/main.luna`
- `tests/fixtures/workspaces/local/core/luna.package`
- `tests/fixtures/workspaces/local/core/src/alternate.luna`
- `tests/fixtures/workspaces/local/core/src/core.luna`
- `tests/fixtures/workspaces/local/luna.lock`
- `tests/fixtures/workspaces/local/luna.workspace`
- `tests/fixtures/workspaces/std_io/app/luna.package`
- `tests/fixtures/workspaces/std_io/app/src/main.luna`
- `tests/fixtures/workspaces/std_io/luna.lock`
- `tests/fixtures/workspaces/std_io/luna.workspace`
- `tests/fixtures/workspaces/std_io/stdin.txt`
- `tests/fragment_lowering_abi.cmake`
- `tests/fragment_plugin_fixture.cpp`
- `tests/fragment_plugin_test.cpp`
- `tests/full_showcase.cmake`
- `tests/gpu_error_boundary_abi.cmake`
- `tests/gpu_target_split.cmake`
- `tests/install_smoke.cmake`
- `tests/iterator_materialized_aot.cmake`
- `tests/iterator_materialized_move_only_aot.cmake`
- `tests/iterator_move_only_aot.cmake`
- `tests/iterator_pipeline_aot.cmake`
- `tests/jit_aot_extended_parity.cmake`
- `tests/jit_aot_parity.cmake`
- `tests/jit_runtime_symbols.cmake`
- `tests/moon_cost_boundaries.cmake`
- `tests/optimization_pipeline.cmake`
- `tests/package_export_abi.cmake`
- `tests/package_manifest_workspace.cmake`
- `tests/package_module_model.cmake`
- `tests/repl_smoke.cmake`
- `tests/result_error_aot.cmake`
- `tests/result_extended_aot.cmake`
- `tests/return_cleanup_abi.cmake`
- `tests/rocm_isa_abi.cmake`
- `tests/rocm_smoke.cmake`
- `tests/runtime_abi_c_compile.c`
- `tests/runtime_abi_test.cpp`
- `tests/runtime_allocation_abi_test.cpp`
- `tests/runtime_application_host_test.cpp`
- `tests/runtime_filesystem_abi_test.cpp`
- `tests/runtime_gpu_error_test.cpp`
- `tests/semantic_regressions.cmake`
- `tests/source_manager_test.cpp`
- `tests/stable_core_parity.cmake`
- `tests/std_io_smoke.cmake`
- `tests/structured_cps_abi.cmake`
- `tools/benchmark_heterogeneous.sh`
- `tools/verify_ecosystem_lock.cmake`
- `tools/verify_release_evidence.js`
<!-- FILE_INVENTORY_END -->
