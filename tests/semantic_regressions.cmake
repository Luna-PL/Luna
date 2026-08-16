# Stable-core semantic regression suite. Source programs deliberately use
# their own integer return values, so success is recognized by the compiler's
# normal completion line rather than by process status alone.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

function(run_luna source output result)
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" run "${LUNA_SOURCE_DIR}/${source}"
        RESULT_VARIABLE command_result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
    )
    set(${output} "${command_output}\n${command_error}" PARENT_SCOPE)
    set(${result} "${command_result}" PARENT_SCOPE)
endfunction()

function(expect_success name source expected)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if(NOT compiler_error EQUAL -1 OR expected_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not complete as expected.\n"
            "Expected text: ${expected}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_success_without name source expected forbidden)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    string(FIND "${transcript}" "${forbidden}" forbidden_at)
    if(NOT compiler_error EQUAL -1 OR expected_at EQUAL -1 OR NOT forbidden_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not complete with the expected side-effect behavior.\n"
            "Expected text: ${expected}\n"
            "Forbidden text: ${forbidden}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_success_with_env name source environment expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${environment}"
                "${LUNA_EXECUTABLE}" run "${LUNA_SOURCE_DIR}/${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
    )
    set(transcript "${command_output}\n${command_error}")
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if(NOT result EQUAL 0 OR NOT compiler_error EQUAL -1 OR expected_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not complete as expected.\n"
            "Expected text: ${expected}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_runtime_failure_with_env name source environment expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${environment}"
                "${LUNA_EXECUTABLE}" run "${LUNA_SOURCE_DIR}/${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error
    )
    set(transcript "${command_output}\n${command_error}")
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if("${result}" STREQUAL "0" OR NOT compiler_error EQUAL -1 OR expected_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not terminate at the expected runtime failure boundary.\n"
            "Expected text: ${expected}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_runtime_failure name source expected)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if("${result}" STREQUAL "0" OR NOT compiler_error EQUAL -1 OR expected_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not terminate at the expected runtime failure boundary.\n"
            "Expected text: ${expected}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_error name source expected)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if(NOT result EQUAL 1 OR compiler_error EQUAL -1 OR expected_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not fail with the expected diagnostic.\n"
            "Expected text: ${expected}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_error_with_source name source expected expected_source)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    string(FIND "${transcript}" "${expected_source}" source_at)
    if(NOT result EQUAL 1 OR compiler_error EQUAL -1 OR expected_at EQUAL -1 OR source_at EQUAL -1)
        message(FATAL_ERROR
            "${name} did not include the expected diagnostic source excerpt.\n"
            "Expected message: ${expected}\n"
            "Expected source: ${expected_source}\n"
            "Process result: ${result}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

function(expect_errors name source)
    run_luna("${source}" transcript result)
    string(FIND "${transcript}" "error[" compiler_error)
    if(NOT result EQUAL 1 OR compiler_error EQUAL -1)
        message(FATAL_ERROR "${name} did not fail during parsing.\nTranscript:\n${transcript}")
    endif()
    foreach(expected IN LISTS ARGN)
        string(FIND "${transcript}" "${expected}" expected_at)
        if(expected_at EQUAL -1)
            message(FATAL_ERROR
                "${name} did not report every expected error.\n"
                "Missing: ${expected}\nTranscript:\n${transcript}")
        endif()
    endforeach()
endfunction()

# Valid programs: basic functions/types, compile-time reflection, closures,
# fragments, metadata selection, trait coherence, package assembly, and the
# portable heterogeneous-compute simulator.
expect_success("minimal function" "examples/minimal.luna" "Program exited with code: 42")
expect_success("operators" "examples/operators.luna" "Program exited with code: 6")
expect_success("comparison operators" "tests/fixtures/comparison_operators.luna" "Program exited with code: 42")
expect_success_without("logical short-circuit" "tests/fixtures/logical_short_circuit.luna" "Program exited with code: 42" "99")
expect_success("signature and auto inference" "examples/inference.luna" "Program exited with code: 42")
expect_success("generic monomorphization" "examples/generic.luna" "Program exited with code: 99")
expect_success("generic body cloning" "tests/fixtures/generic_body_cloning.luna" "Program exited with code: 42")
expect_success("compile-time reflection" "examples/compile_time.luna" "Program exited with code: 42")
expect_error("named products are nominal by default" "tests/fixtures/structural_type_equivalence.luna" "Pixel and Point are different types")
expect_success("type domains and stable identity reflection" "tests/fixtures/type_domains_reflection.luna" "Program exited with code: 42")
expect_success("explicit type relations" "tests/fixtures/type_relations.luna" "Program exited with code: 42")
expect_success("anonymous record type, value, layout, and constraint" "tests/fixtures/anonymous_records.luna" "1\n40\nProgram exited with code: 42")
expect_success("anonymous record recursively cleans owned fields" "tests/fixtures/anonymous_record_owned_field.luna" "Program exited with code: 42")
expect_success("explicit named record construction and anonymous projection" "tests/fixtures/named_record_construction.luna" "2\n40\nProgram exited with code: 42")
expect_success("record and statement-block grammar contexts" "tests/fixtures/record_block_context.luna" "Program exited with code: 42")
expect_success("usage blocks, overrides, nesting, binders, and lambda reset" "tests/fixtures/usage_blocks.luna" "8\n42\nProgram exited with code: 42")
expect_error("named sums are nominal by default" "tests/fixtures/structural_enum_equivalence.luna" "Second and First are different types")
expect_error("trait coherence follows nominal identity" "tests/fixtures/structural_trait_coherence.luna" "does not satisfy trait 'Tagged'")
expect_success("closure" "examples/closure.luna" "Program exited with code: 42")
expect_success("fragments" "examples/fragments.luna" "Program exited with code: 0")
expect_success_without("explicit fragment contracts" "tests/fixtures/fragment_contracts.luna" "41" "99")
expect_success("context retains a private linear guard" "tests/fixtures/context_linear_guard.luna" "Program exited with code: 0")
expect_success("context abort preserves untouched outer resource" "tests/fixtures/context_abort_preserves_outer_resource.luna" "42")
expect_success("dynamic fragments default selection" "tests/fixtures/dynamic_fragments.luna" "41")
expect_success_with_env("dynamic fragments runtime selection" "tests/fixtures/dynamic_fragments.luna" "LUNA_FRAGMENT_PIPELINE=audit" "43")
expect_runtime_failure_with_env("dynamic context rejects external plugin fallback" "tests/fixtures/dynamic_fragments.luna" "LUNA_FRAGMENT_PIPELINE=missing" "dynamic fragment selection")
expect_success("metadata selector" "examples/versioning.luna" "Program exited with code: 20")
expect_success("user-defined selector traversal" "tests/fixtures/selector_user_logic.luna" "Program exited with code: 30")
expect_success("named compile-time concepts" "tests/fixtures/concepts.luna" "Program exited with code: 42")
expect_success("static declaration reflection" "tests/fixtures/static_declaration_reflection.luna" "Program exited with code: 42")
expect_success("dynamic metadata selector" "examples/dynamic_select.luna" "Program exited with code: 20")
expect_success("trait metadata" "examples/trait_versioning.luna" "Program exited with code: 0")
expect_success("nominal trait metadata" "examples/trait_versioned_nominal.luna" "Program exited with code: 0")
expect_success("package exports" "tests/fixtures/packages/exported_package" "Program exited with code: 0")
expect_success("package and module headers" "tests/fixtures/packages/module_headers" "Program exited with code: 42")
expect_success("C FFI" "examples/ffi.luna" "hello from C FFI")
expect_success("owning FFI return" "tests/fixtures/ffi_owning_return.luna" "Program exited with code: 0")
expect_success("heterogeneous simulator" "examples/heterogeneous.luna" "Program exited with code: 0")
expect_success("metadata-tagged heterogeneous kernel" "examples/heterogeneous_versioned.luna" "Program exited with code: 0")
expect_success("moved launch event" "examples/heterogeneous_move_event.luna" "Program exited with code: 0")
expect_success("heterogeneous bulk host-device transfer" "tests/fixtures/heterogeneous_bulk_transfer.luna" "Program exited with code: 42")
expect_success("if consumes linear value on every path" "tests/fixtures/ownership_if_all_paths_consume.luna" "Program exited with code: 0")
expect_success("returning branch has separate ownership path" "tests/fixtures/ownership_return_path_valid.luna" "Program exited with code: 0")
expect_success("all returning paths consume linear value" "tests/fixtures/ownership_all_return_paths_consume.luna" "Program exited with code: 0")
expect_success("loop-local resource ownership" "tests/fixtures/ownership_loop_local_resource_valid.luna" "Program exited with code: 0")
expect_success("unreachable code after return" "tests/fixtures/ownership_unreachable_after_return.luna" "Program exited with code: 0")
expect_success("heap parameters borrow caller ownership" "tests/fixtures/ownership_heap_parameter_borrow.luna" "Program exited with code: 42")
expect_success("safe fixed arrays" "tests/fixtures/safe_arrays.luna" "Program exited with code: 40")
expect_success("borrowed safe slice" "tests/fixtures/slice_borrow.luna" "Program exited with code: 30")
expect_success("empty tail slice" "tests/fixtures/slice_empty_tail.luna" "Program exited with code: 0")
expect_success("compiler-known Drop trait" "tests/fixtures/drop_intrinsic.luna" "92\n91")
expect_success("named products recursively clean owned fields" "tests/fixtures/resource_recursive_named.luna" "31\n32")
expect_success("generic Drop composes with recursive field cleanup" "tests/fixtures/resource_generic_drop.luna" "41\n42")
expect_success("Drop contract is independent of declaration order" "tests/fixtures/resource_drop_after_use.luna" "51")
expect_error("Drop requires an infallible unit signature" "tests/fixtures/resource_drop_signature_invalid.luna" "Drop::drop must return unit")
expect_error("Drop Resource cannot be weakened to Copy" "tests/fixtures/resource_drop_copy_weaken_invalid.luna" "declares copy usage, but its type or initializer requires at least affine")
expect_error("generic Drop rejects layout-dependent unspecialized bodies" "tests/fixtures/resource_generic_drop_layout_invalid.luna" "generic Drop target has type-parameter-dependent storage layout")
expect_success("Result construction, inspection, and extraction" "tests/fixtures/result_basic.luna" "16\n10\n11\n13\n12")
expect_success("Result match is exhaustive and binds both payloads" "tests/fixtures/result_match.luna" "40\n41\n50\n52")
expect_success("Result match transfers and cleans an active resource payload" "tests/fixtures/result_match_resource.luna" "61\n61\n62")
expect_success("general enum matching and frozen inline layout" "tests/fixtures/enum_match.luna" "16\n1\n42\n42")
expect_success("enum match transfers and cleans an active resource payload" "tests/fixtures/enum_match_resource.luna" "71\n71")
expect_success("Result propagation cleans both paths" "tests/fixtures/result_propagation.luna" "70\n43\n70\n7")
expect_success("question mark applies one static From conversion" "tests/fixtures/result_from_conversion.luna" "42\n7\nconverted")
expect_success("From consumes and cleans a move-only source error" "tests/fixtures/result_from_resource.luna" "71\n71")
expect_success("Result drops only its active resource payload" "tests/fixtures/result_resource_cleanup.luna" "81\n82")
expect_success("formal never type and diverging calls" "tests/fixtures/never_type.luna" "never\n0\nProgram exited with code: 42")
expect_success("fused iterator pipelines and mutable iteration" "tests/fixtures/iterator_pipeline.luna" "14\n20\n30\n40\n3\n1\n2\n3\n4\n5\n6\n7\n70\n8\n9\n90\n11\n15\nProgram exited with code: 14")
expect_success("iterator adapters accept capturing closures" "tests/fixtures/iterator_closure_callback.luna" "Program exited with code: 14")
expect_success("borrowed slice iteration" "tests/fixtures/iterator_slice.luna" "20\n30\n40\nProgram exited with code: 3")
expect_error("read-only slice rejects mutable iteration" "tests/fixtures/iterator_slice_mut_invalid.luna" "`iter_mut` requires an owning array receiver")
expect_error("read-only slice rejects consuming iteration" "tests/fixtures/iterator_slice_into_invalid.luna" "`into_iter` requires an owning array receiver")
expect_success("move-only consuming arrays keep per-element drop state" "tests/fixtures/iterator_move_only_array.luna" "131\n231\n141\n142\n1\n143\n1\n151\n151\n161\n161\n171\n171\n181\n182\n184\n184")
expect_runtime_failure("explicit panic" "tests/fixtures/panic.luna" "Luna panic: intentional panic")
expect_runtime_failure("unwrap failure panics" "tests/fixtures/result_unwrap_panic.luna" "Luna panic: called unwrap on Err")

# Negative programs: nominal typing, inference, metadata selection, fragment
# replay, package assembly, linear resources, and in-flight device ownership.
expect_error_with_source("parse binding name" "tests/fixtures/parse_missing_binding_name.luna" "error[parse/PAR0001]: expected variable name after 'let'" "let = 1;")
expect_error("post-let usage qualifier is removed" "tests/fixtures/post_let_usage_invalid.luna" "expected variable name after 'let'")
expect_errors("top-level parser recovery" "tests/fixtures/parse_multiple_declarations_invalid.luna"
    "expected a declaration, found 'nonsense'"
    "expected a declaration, found 'also_wrong'")
expect_error("removed nominal modifier" "tests/fixtures/nominal_modifier_invalid.luna" "expected a declaration, found 'nominal'")
expect_errors("package parser recovery across files" "tests/fixtures/packages/multiple_parse_errors"
    "expected a declaration, found 'first_problem'"
    "expected a declaration, found 'second_problem'")
expect_error("nominal type mismatch" "examples/adt_error.luna" "Pixel and Point are different types")
expect_success("recursive nominal type through raw indirection" "tests/fixtures/recursive_structural_type_invalid.luna" "Program exited with code: 0")
expect_error("different named field orders remain nominally distinct" "tests/fixtures/structural_field_order_invalid.luna" "Second and First are different types")
expect_error("duplicate anonymous record field" "tests/fixtures/anonymous_record_duplicate_invalid.luna" "duplicate record field 'x'")
expect_error("anonymous record partial move waits for field drop flags" "tests/fixtures/anonymous_record_partial_move_invalid.luna" "partial move from anonymous record 'record' is not yet supported")
expect_error("ordinary blocks inherit a linear usage default" "tests/fixtures/usage_block_linear_unconsumed_invalid.luna" "Linear variable 'token' was not consumed")
expect_error("match payload bindings inherit a linear usage default" "tests/fixtures/usage_block_pattern_unconsumed_invalid.luna" "Linear variable 'item' was not consumed")
expect_error("for bindings inherit a linear usage default" "tests/fixtures/usage_block_loop_unconsumed_invalid.luna" "Linear iterator item 'item' must be consumed")
expect_error("explicit usage cannot weaken an inherent resource contract" "tests/fixtures/usage_contract_weaken_invalid.luna" "declares copy usage, but its type or initializer requires at least affine")
expect_error("named record construction requires every field" "tests/fixtures/named_record_missing_field_invalid.luna" "is missing field 'y'")
expect_error("named record construction rejects unknown fields" "tests/fixtures/named_record_unknown_field_invalid.luna" "has no field 'y'")
expect_error("inline where predicate rejection" "tests/fixtures/inline_where_not_satisfied_invalid.luna" "inline where predicate is not satisfied")
expect_error("unresolved inference" "examples/inference_error.luna" "Could not infer parameter 'value'")
expect_error("generic argument count" "tests/fixtures/generic_argument_count_invalid.luna" "Argument count mismatch for 'identity'")
expect_success("no-capture iterator recipes materialize on the stack" "tests/fixtures/iterator_materialized.luna" "20\n30\n1\n12\n7\n8")
expect_success("move-only materialized iterator drop state" "tests/fixtures/iterator_materialized_move_only.luna" "1201\n201\n202\n203\n211\n212\n221\n222\n7\n231\n232\n233\n231\n241\n242\n9\n1241\n241\n1242\n242\n10")
expect_error("iterator map input type" "tests/fixtures/iterator_map_type_invalid.luna" "Type constraint failed in iterator map input")
expect_error("ordinary value cannot inhabit never" "tests/fixtures/never_return_value_invalid.luna" "Type constraint failed in return statement")
expect_error("mutable iterator holds exclusive source borrow" "tests/fixtures/iterator_mut_borrow_conflict_invalid.luna" "while an overlapping place is mutably borrowed")
expect_error("missing non-unit return path" "tests/fixtures/missing_return_invalid.luna" "function 'classify' may finish without returning 'i32'")
expect_error("non-numeric relational comparison" "tests/fixtures/comparison_non_numeric_invalid.luna" "left operand of comparison expression must be numeric, got bool")
expect_error_with_source("non-constant const binding" "tests/fixtures/constexpr_nonconstant.luna" "const binding 'value' is not a compile-time expression" "const let value = runtime_value();")
expect_error("reflection index out of range" "tests/fixtures/reflection_index_out_of_range.luna" "type_field_name index 2 is out of range")
expect_error("metadata selector has no solution" "examples/versioning_invalid.luna" "selector returned no legal declaration")
expect_error("selector cannot escape its candidate view" "tests/fixtures/selector_outside_view_invalid.luna" "outside its supplied candidate view")
expect_error("named concept is not satisfied" "tests/fixtures/concept_not_satisfied_invalid.luna" "constraint 'main::Tiny<string>' is not satisfied")
expect_error("removed postfix versioning syntax" "examples/trait_versioning_invalid.luna" "postfix `@tag(...)` versioning has been removed")
expect_error("incomplete trait implementation" "examples/trait_versioning_incomplete_invalid.luna" "is missing method 'transform'")
expect_error("fragment replay captures linear value" "examples/fragment_multishot_invalid.luna" "captured linear value 'buffer' is not replayable")
expect_error("fragment replay frees captured value" "examples/fragment_multishot_free_invalid.luna" "continuation consumes or frees captured state")
expect_error("dynamic apply requires dynamic slot" "tests/fixtures/dynamic_apply_static_slot_invalid.luna" "is static; declare it with `dynamic slot`")
expect_error("interceptor cannot resume" "tests/fixtures/interceptor_resume_invalid.luna" "`resume()` is not allowed in an interceptor")
expect_success_without("context without resume is implicit abort" "tests/fixtures/context_missing_control_invalid.luna" "Program exited with code: 0" "error[")
expect_success_without("partial context resume permits implicit abort" "tests/fixtures/context_partial_resume_invalid.luna" "Program exited with code: 0" "error[")
expect_success_without("fragment return ends fragment" "tests/fixtures/context_return_ends_fragment_valid.luna" "Program exited with code: 0" "42")
expect_success("loop-local slot plugin default" "examples/slot_plugins/loop_plugins.luna" "100")
expect_success_with_env("loop-local slot plugin selection" "examples/slot_plugins/loop_plugins.luna" "LUNA_FRAGMENT_HOOK=audit" "200")
expect_error("context abort ownership merge" "tests/fixtures/context_abort_ownership_mismatch_invalid.luna" "linear resource 'data' is consumed on only some paths")
expect_error("context abort consumes local linear state" "tests/fixtures/context_abort_leaks_local_invalid.luna" "must be consumed before aborting the fragment")
expect_success("structured CPS continuation return" "tests/fixtures/context_continuation_return_invalid.luna" "Program exited with code: 1")
expect_error("single-shot abort cannot follow resume" "tests/fixtures/context_abort_after_resume_invalid.luna" "cannot abort after resume()")
expect_error("legacy fragment is ambiguous" "tests/fixtures/legacy_fragment_invalid.luna" "`fragment` is ambiguous")
expect_error("slot and fragment contracts match" "tests/fixtures/slot_fragment_contract_mismatch_invalid.luna" "does not match slot 'hook' interceptor/context and once/many contract")
expect_error("slot declares fragment contract" "tests/fixtures/slot_missing_contract_invalid.luna" "slot must declare its fragment contract")
expect_error("slot declares once/many contract" "tests/fixtures/slot_cardinality_contract_mismatch_invalid.luna" "does not match slot 'hook' interceptor/context and once/many contract")
expect_error("apply checks known slot contract eagerly" "tests/fixtures/apply_contract_checked_eagerly_invalid.luna" "does not match slot 'hook' interceptor/context and once/many contract")
expect_error("dynamic candidates share explicit contract" "tests/fixtures/dynamic_candidate_contract_mismatch_invalid.luna" "dynamic candidate 'inspect' does not match slot 'hook' contract")
expect_error("metadata does not bypass fragment uniqueness" "tests/fixtures/versioned_fragment_contract_change_invalid.luna" "duplicate fragment declaration 'hook'")
expect_error("package name mismatch" "tests/fixtures/packages/mismatched_package" "does not match 'mismatch_a'")
expect_error("package alias collision" "tests/fixtures/packages/alias_collision" "package alias 'log' refers to both")
expect_error("package self using" "tests/fixtures/packages/self_using" "package cannot use itself")
expect_error("package using requires alias" "tests/fixtures/package_using_missing_alias_invalid.luna" "package using declaration requires a local alias")
expect_error_with_source("duplicate exported package symbol" "tests/fixtures/packages/duplicate_export" "Duplicate package declaration 'shared'" "export fn shared() -> i32 {")
expect_error_with_source("duplicate metadata declaration identity" "tests/fixtures/packages/duplicate_version" "Duplicate package declaration 'greet'" "@version(1, 0, 0)")
expect_error("extern export boundary" "tests/fixtures/invalid_export.luna" "An extern function cannot also be exported")
expect_error("unsupported FFI ABI" "tests/fixtures/ffi_unsupported_abi_invalid.luna" "error[semantic/SEM0101]: Unsupported ABI 'stdcall' for function 'foreign'")
expect_error("generic FFI declaration" "tests/fixtures/ffi_generic_invalid.luna" "C ABI function cannot be generic: 'identity'")
expect_error("unsupported FFI type" "tests/fixtures/ffi_unsupported_type_invalid.luna" "Unsupported FFI type string in parameter 'value' of 'display'")
expect_error("Result cannot cross the raw C ABI" "tests/fixtures/ffi_result_boundary_invalid.luna" "Unsupported FFI type Result<i32, i32> in return type of FFI function 'foreign_result'")
expect_error("ignored owning FFI return" "tests/fixtures/ffi_owning_return_ignored_invalid.luna" "owning result of FFI call must be bound to a variable and consumed")
expect_error("invalid owning FFI return type" "tests/fixtures/ffi_owning_return_type_invalid.luna" "owning FFI return of 'bad_allocator' must use `linear raw<T>`")
expect_error_with_source("in-flight device buffer borrow" "examples/heterogeneous_inflight_invalid.luna" "Cannot borrow device buffer 'data' while a launch is in flight" "gpu_store_i32(borrow mut data, 0, 2);")
expect_error("bulk upload requires mutable buffer borrow" "tests/fixtures/heterogeneous_bulk_transfer_invalid.luna" "requires `borrow mut buffer`")
expect_error("unawaited launch event" "examples/heterogeneous_unawaited_invalid.luna" "was not awaited before returning")
expect_errors("kernel host-control effects" "tests/fixtures/kernel_host_effects_invalid.luna"
    "kernel body may not use slot invocation"
    "kernel body may not use apply binding"
    "kernel body may not use resume()"
    "kernel body may not use abort()"
    "kernel body may not use await"
    "kernel body may not use free"
    "kernel body may not allocate heap memory with `new`")
expect_error("conditional partial linear consumption" "tests/fixtures/ownership_if_partial_consume_invalid.luna" "error[ownership/OWN0201]: linear resource 'buffer' is consumed on only some paths through `if`")
expect_error("loop consumes outer linear resource" "tests/fixtures/ownership_loop_consumes_outer_invalid.luna" "loop body changes ownership, borrow, or in-flight state of 'buffer'")
expect_error("move-only array literal requires explicit element transfer" "tests/fixtures/array_move_element_invalid.luna" "move-only array elements must be moved explicitly")
expect_error("consuming array iteration invalidates its source" "tests/fixtures/iterator_move_only_array_use_after_invalid.luna" "use after move")
expect_error("iterator map requires owning move-only inputs" "tests/fixtures/iterator_map_move_only_input_invalid.luna" "map transform must own a move-only input")
expect_error("iterator filter borrows move-only inputs" "tests/fixtures/iterator_filter_move_only_owning_invalid.luna" "filter predicate must borrow a move-only item")
expect_error("iterator terminal does not hide linear state" "tests/fixtures/iterator_count_move_only_invalid.luna" "linear iterator terminal state cannot be hidden")
expect_error("iterator terminal invalidates its source" "tests/fixtures/iterator_terminal_use_after_invalid.luna" "use after move of 'values[0].marker'")
expect_error("materialized iterator is single-consumption" "tests/fixtures/iterator_materialized_twice_invalid.luna" "use after move of 'pending'")
expect_error("materialized mutable iterator retains its source loan" "tests/fixtures/iterator_materialized_borrow_invalid.luna" "while an overlapping place is mutably borrowed")
expect_error("owning materialized iterator invalidates its source" "tests/fixtures/iterator_materialized_source_use_after_invalid.luna" "use after move of 'values[0].marker'")
expect_error("materialized iterator does not hide a linear source" "tests/fixtures/iterator_materialized_linear_source_invalid.luna" "cannot hide linear source 'values'")
expect_error("iterator for_each requires owning move-only inputs" "tests/fixtures/iterator_for_each_move_only_invalid.luna" "for_each action must own a move-only item")
expect_error("iterator fold requires owning move-only inputs" "tests/fixtures/iterator_fold_move_only_invalid.luna" "fold reducer must own a move-only item")
expect_error("move-only fold initial value requires move" "tests/fixtures/iterator_fold_accumulator_move_invalid.luna" "move-only fold accumulator 'initial' must be moved explicitly")
expect_error("move-only fold reducer owns accumulator" "tests/fixtures/iterator_fold_accumulator_borrow_invalid.luna" "fold reducer must own a move-only accumulator")
expect_error("fold does not hide a linear accumulator" "tests/fixtures/iterator_fold_accumulator_linear_invalid.luna" "linear fold accumulator cannot be hidden")
expect_error("move-only fold result requires an owner" "tests/fixtures/iterator_fold_accumulator_ignored_invalid.luna" "all move-only results require an explicit owner")
expect_success("lambda copies a Copy capture" "tests/fixtures/lambda_capture_copy.luna" "Program exited with code: 42")
expect_success("lambda preserves every Copy capture" "tests/fixtures/lambda_capture_multiple.luna" "Program exited with code: 42")
expect_success("lambda moves an affine capture" "tests/fixtures/lambda_capture_affine_move.luna" "luna\nProgram exited with code: 1")
expect_error("lambda rejects use of a moved affine capture" "tests/fixtures/lambda_capture_affine_use_after_move.luna" "use after move of 'captured'")
expect_error("lambda rejects a borrowed capture" "tests/fixtures/lambda_capture_borrowed_invalid.luna" "lambda capture of borrowed binding 'left' is not yet supported")
expect_success("lambda propagates a transitive capture" "tests/fixtures/lambda_nested_transitive_capture.luna" "Program exited with code: 42")
expect_success("lambda propagates a partial capture set" "tests/fixtures/lambda_nested_partial_capture.luna" "Program exited with code: 21")
expect_success("lambda combines parameter and transitive capture" "tests/fixtures/lambda_nested_parameter_capture.luna" "Program exited with code: 9")
expect_success("lambda preserves outer binding after shadowing" "tests/fixtures/lambda_capture_shadow.luna" "Program exited with code: 5")
expect_success("lambda parameter shadows an outer capture" "tests/fixtures/lambda_capture_param_shadow.luna" "Program exited with code: 42")
expect_success("lambda return values stay callable closures" "tests/fixtures/lambda_return_closure.luna" "Program exited with code: 42")
expect_error("lambda linear parameters must be consumed" "tests/fixtures/lambda_linear_parameter_invalid.luna" "Linear variable 'value' was not consumed")
expect_error("returning path leaks linear resource" "tests/fixtures/ownership_return_path_leaks_invalid.luna" "Linear variable 'buffer' was not consumed before returning")
expect_error("safe array static bounds" "tests/fixtures/safe_array_static_bounds_invalid.luna" "array index 2 is outside array length 2")
expect_error("safe array homogeneous elements" "tests/fixtures/safe_array_wrong_element_invalid.luna" "Type constraint failed in array element")
expect_error("slice prevents source write" "tests/fixtures/slice_write_source_invalid.luna" "Cannot assign to 'values' while it is borrowed")
expect_success("affine values may be discarded" "tests/fixtures/ownership_affine_drop.luna" "Program exited with code: 7")
expect_error("affine owning parameter requires move" "tests/fixtures/ownership_affine_requires_move_invalid.luna" "affine value 'token' must be moved explicitly when passed to an owning call")
expect_error("partial move invalidates only selected place" "tests/fixtures/ownership_partial_move_invalid.luna" "use of moved place 'bundle.first'")
expect_success("disjoint field borrows" "tests/fixtures/ownership_disjoint_field_borrows.luna" "Program exited with code: 0")
expect_error("overlapping field borrows" "tests/fixtures/ownership_overlapping_field_borrows_invalid.luna" "overlapping place is borrowed")
expect_error("partial move branch state" "tests/fixtures/ownership_partial_move_branch_invalid.luna" "ownership state of 'bundle' differs across paths through `if`")
expect_error("removed rc/arc construction syntax" "tests/fixtures/rc_arc.luna" "expected ';' after let binding, found 'new'")
expect_error("question mark requires Result" "tests/fixtures/result_try_non_result_invalid.luna" "`?` requires Result<T, E>, got i32")
expect_error("question mark requires Result-returning function" "tests/fixtures/result_try_non_result_function_invalid.luna" "`?` requires the enclosing function to return Result")
expect_error("question mark requires a static error conversion" "tests/fixtures/result_try_error_mismatch_invalid.luna" "implement `From<string> for i32`")
expect_error("From conversion signature is checked" "tests/fixtures/result_from_signature_invalid.luna" "From::from requires exactly one parameter of type 'i32'")
expect_error("From owns a move-only source error" "tests/fixtures/result_from_borrowed_source_invalid.luna" "From::from must take ownership of move-only source 'BorrowedSourceError'")
expect_error("question mark cannot cross fragment boundary" "tests/fixtures/result_try_fragment_invalid.luna" "`?` may not propagate across a fragment/slot boundary")
expect_error("Result constructor requires enough type context" "tests/fixtures/result_ambiguous_constructor_invalid.luna" "Could not infer type arguments of 'Ok'")
expect_error("Result match requires both variants" "tests/fixtures/result_match_non_exhaustive_invalid.luna" "Result match must contain exactly one `Ok` arm and one `Err` arm")
expect_error("enum match requires every variant" "tests/fixtures/enum_match_non_exhaustive_invalid.luna" "is not exhaustive; missing variant(s): Number")
expect_error("enum match rejects duplicate variants" "tests/fixtures/enum_match_duplicate_invalid.luna" "duplicate match arm for variant 'Stop'")
expect_error("enum match checks payload arity" "tests/fixtures/enum_match_arity_invalid.luna" "variant 'Number' expects 1 payload binding(s), got 2")
expect_success("Result supports nested and aggregate payloads" "tests/fixtures/result_payload_abi_invalid.luna" "41\n24\nProgram exited with code: 30")
expect_error("panic requires a text message" "tests/fixtures/panic_message_type_invalid.luna" "panic message must be string or cstr")
expect_error("temporary print rejects unsupported values" "tests/fixtures/print_unsupported_type_invalid.luna" "temporary print supports only i32, string, or cstr")
