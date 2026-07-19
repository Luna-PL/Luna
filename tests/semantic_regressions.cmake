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
# fragments, version selection, trait coherence, package assembly, and the
# portable heterogeneous-compute simulator.
expect_success("minimal function" "examples/minimal.luna" "Program exited with code: 42")
expect_success("operators" "examples/operators.luna" "Program exited with code: 6")
expect_success("comparison operators" "tests/fixtures/comparison_operators.luna" "Program exited with code: 42")
expect_success_without("logical short-circuit" "tests/fixtures/logical_short_circuit.luna" "Program exited with code: 42" "99")
expect_success("generic monomorphization" "examples/generic.luna" "Program exited with code: 99")
expect_success("compile-time reflection" "examples/compile_time.luna" "Program exited with code: 46")
expect_success("closure" "examples/closure.luna" "Program exited with code: 42")
expect_success("fragments" "examples/fragments.luna" "Program exited with code: 0")
expect_success_without("explicit fragment contracts" "tests/fixtures/fragment_contracts.luna" "41" "99")
expect_success("context retains a private linear guard" "tests/fixtures/context_linear_guard.luna" "Program exited with code: 0")
expect_success("context abort preserves untouched outer resource" "tests/fixtures/context_abort_preserves_outer_resource.luna" "42")
expect_success("dynamic fragments default selection" "tests/fixtures/dynamic_fragments.luna" "41")
expect_success_with_env("dynamic fragments runtime selection" "tests/fixtures/dynamic_fragments.luna" "LUNA_FRAGMENT_PIPELINE=audit" "43")
expect_success("declaration versioning" "examples/versioning.luna" "Program exited with code: 20")
expect_success("trait versioning" "examples/trait_versioning.luna" "Program exited with code: 0")
expect_success("nominal trait versioning" "examples/trait_versioned_nominal.luna" "Program exited with code: 0")
expect_success("package exports" "tests/fixtures/packages/exported_package" "Program exited with code: 0")
expect_success("C FFI" "examples/ffi.luna" "hello from C FFI")
expect_success("owning FFI return" "tests/fixtures/ffi_owning_return.luna" "Program exited with code: 0")
expect_success("heterogeneous simulator" "examples/heterogeneous.luna" "Program exited with code: 0")
expect_success("versioned heterogeneous kernel" "examples/heterogeneous_versioned.luna" "Program exited with code: 0")
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

# Negative programs: nominal typing, inference, version resolution, fragment
# replay, package assembly, linear resources, and in-flight device ownership.
expect_error_with_source("parse binding name" "tests/fixtures/parse_missing_binding_name.luna" "error[parse/PAR0001]: expected variable name after 'let'" "let = 1;")
expect_errors("top-level parser recovery" "tests/fixtures/parse_multiple_declarations_invalid.luna"
    "expected a declaration, found 'nonsense'"
    "expected a declaration, found 'also_wrong'")
expect_errors("package parser recovery across files" "tests/fixtures/packages/multiple_parse_errors"
    "expected a declaration, found 'first_problem'"
    "expected a declaration, found 'second_problem'")
expect_error("nominal type mismatch" "examples/adt_error.luna" "Pixel and Point are different types")
expect_error("unresolved inference" "examples/inference_error.luna" "Could not infer parameter 'value'")
expect_error("generic argument count" "tests/fixtures/generic_argument_count_invalid.luna" "Argument count mismatch for 'identity'")
expect_error("missing non-unit return path" "tests/fixtures/missing_return_invalid.luna" "function 'classify' may finish without returning 'i32'")
expect_error("non-numeric relational comparison" "tests/fixtures/comparison_non_numeric_invalid.luna" "left operand of comparison expression must be numeric, got bool")
expect_error_with_source("non-constant const binding" "tests/fixtures/constexpr_nonconstant.luna" "const binding 'value' is not a compile-time expression" "const let value = runtime_value();")
expect_error("reflection index out of range" "tests/fixtures/reflection_index_out_of_range.luna" "type_field_name index 2 is out of range")
expect_error("missing exact function version" "examples/versioning_invalid.luna" "has no `@stable(1.1.0)` declaration")
expect_error("unselected trait version" "examples/trait_versioning_invalid.luna" "trait 'Transform' is versioned; select a tag explicitly")
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
expect_error("versioned fragment preserves contract" "tests/fixtures/versioned_fragment_contract_change_invalid.luna" "changes its interceptor/context or once/many contract")
expect_error("package name mismatch" "tests/fixtures/packages/mismatched_package" "does not match 'mismatch_a'")
expect_error_with_source("duplicate exported package symbol" "tests/fixtures/packages/duplicate_export" "Duplicate package declaration 'shared'" "export fn shared() -> i32 {")
expect_error_with_source("duplicate versioned package symbol" "tests/fixtures/packages/duplicate_version" "Duplicate package declaration 'greet@stable(1.0.0)'" "fn greet @stable(1.0.0)() -> i32 {")
expect_error("extern export boundary" "tests/fixtures/invalid_export.luna" "An extern function cannot also be exported")
expect_error("unsupported FFI ABI" "tests/fixtures/ffi_unsupported_abi_invalid.luna" "error[semantic/SEM0101]: Unsupported ABI 'stdcall' for function 'foreign'")
expect_error("generic FFI declaration" "tests/fixtures/ffi_generic_invalid.luna" "C ABI function cannot be generic: 'identity'")
expect_error("unsupported FFI type" "tests/fixtures/ffi_unsupported_type_invalid.luna" "Unsupported FFI type string in parameter 'value' of 'display'")
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
expect_error("returning path leaks linear resource" "tests/fixtures/ownership_return_path_leaks_invalid.luna" "Linear variable 'buffer' was not consumed before returning")
expect_error("safe array static bounds" "tests/fixtures/safe_array_static_bounds_invalid.luna" "array index 2 is outside array length 2")
expect_error("safe array homogeneous elements" "tests/fixtures/safe_array_wrong_element_invalid.luna" "Type constraint failed in array element")
expect_error("slice prevents source write" "tests/fixtures/slice_write_source_invalid.luna" "Cannot assign to 'values' while it is borrowed")
