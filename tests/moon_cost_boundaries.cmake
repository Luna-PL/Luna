if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/moon-cost-boundaries")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/kernel_unused.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/examples/dynamic_select.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/selector_user_logic.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/concepts.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/generic_instance_reuse.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/structural_generic_instance_reuse.luna" DESTINATION "${work_dir}")

function(build_case source moon_file)
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${source}" -O0
                --emit-moonir "${moon_file}" --moon-cost-report ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        file(REMOVE_RECURSE "${work_dir}")
        message(FATAL_ERROR "cost-boundary build failed:\n${output}\n${error}")
    endif()
    set(last_output "${output}\n${error}" PARENT_SCOPE)
endfunction()

set(kernel_source "${work_dir}/kernel_unused.luna")
set(kernel_ir "${kernel_source}.ll")
build_case("${kernel_source}" "${work_dir}/kernel-default.moonir")
file(READ "${kernel_ir}" default_ir)
file(READ "${work_dir}/kernel-default.moonir" default_moon)
string(FIND "${default_ir}" "rt_gpu_" default_gpu)
string(FIND "${default_ir}" "unused_kernel" default_kernel)
string(FIND "${default_moon}" "deferred_recipe" deferred_recipe)
if(NOT default_gpu EQUAL -1 OR NOT default_kernel EQUAL -1 OR deferred_recipe EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "unused kernel paid a backend/runtime cost\n${default_ir}\n${default_moon}")
endif()

build_case("${kernel_source}" "${work_dir}/kernel-reserved.moonir"
           --reserve-kernel-runtime)
file(READ "${kernel_ir}" reserved_ir)
file(READ "${work_dir}/kernel-reserved.moonir" reserved_moon)
string(FIND "${reserved_ir}" "rt_gpu_initialize" reserved_gpu)
string(FIND "${reserved_ir}" "define void @unused_kernel" reserved_kernel)
string(FIND "${reserved_moon}" "kernel_runtime_reserved" reserved_feature)
if(reserved_gpu EQUAL -1 OR reserved_kernel EQUAL -1 OR reserved_feature EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "explicit kernel reservation did not materialize its audited cost")
endif()

set(dynamic_source "${work_dir}/dynamic_select.luna")
build_case("${dynamic_source}" "${work_dir}/dynamic.moonir")
file(READ "${dynamic_source}.ll" dynamic_ir)
file(READ "${work_dir}/dynamic.moonir" dynamic_moon)
string(FIND "${dynamic_ir}" "__moon_runtime_registry_" runtime_registry)
string(FIND "${dynamic_ir}" "dynamic.select.unique" dynamic_binding)
string(FIND "${dynamic_ir}" "rt_gpu_" dynamic_gpu)
string(FIND "${dynamic_moon}" "dynamic_select" dynamic_feature)
if(runtime_registry EQUAL -1 OR dynamic_binding EQUAL -1 OR
   NOT dynamic_gpu EQUAL -1 OR dynamic_feature EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "dynamic select/runtime descriptor cost boundary is incorrect")
endif()

set(static_selector_source "${work_dir}/selector_user_logic.luna")
build_case("${static_selector_source}" "${work_dir}/static-selector.moonir")
file(READ "${static_selector_source}.ll" static_selector_ir)
file(READ "${work_dir}/static-selector.moonir" static_selector_moon)
string(FIND "${static_selector_ir}" "choose_release" static_selector_machine_code)
string(FIND "${static_selector_ir}" "__moon_runtime_registry_" static_selector_registry)
string(FIND "${static_selector_moon}" "choose_release" static_selector_protocol)
string(FIND "${static_selector_moon}" "declaration_view" static_selector_view)
string(FIND "${static_selector_moon}" "dynamic_select" static_selector_dynamic)
if(NOT static_selector_machine_code EQUAL -1 OR
   NOT static_selector_registry EQUAL -1 OR
   NOT static_selector_protocol EQUAL -1 OR
   NOT static_selector_view EQUAL -1 OR
   NOT static_selector_dynamic EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "static selector/view protocol was not fully erased\n${static_selector_moon}")
endif()

set(concept_source "${work_dir}/concepts.luna")
build_case("${concept_source}" "${work_dir}/concepts.moonir")
file(READ "${concept_source}.ll" concept_ir)
file(READ "${work_dir}/concepts.moonir" concept_moon)
string(FIND "${concept_ir}" "SmallValue" concept_machine_code)
string(FIND "${concept_moon}" "SmallValue" concept_ir_declaration)
string(FIND "${concept_moon}" "PlainSmallValue" composed_concept_ir_declaration)
string(FIND "${concept_moon}" "runtime" concept_runtime)
if(NOT concept_machine_code EQUAL -1 OR
   NOT concept_ir_declaration EQUAL -1 OR
   NOT composed_concept_ir_declaration EQUAL -1 OR
   NOT concept_runtime EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "compile-time constraint leaked into generated artifacts\n${concept_moon}")
endif()

set(generic_source "${work_dir}/generic_instance_reuse.luna")
build_case("${generic_source}" "${work_dir}/generic.moonir")
string(REGEX MATCHALL "\\[generic_instantiation\\]" generic_costs "${last_output}")
list(LENGTH generic_costs generic_cost_count)
file(READ "${work_dir}/generic.moonir" generic_moon)
string(FIND "${generic_moon}" "__moon_inst_" stable_instance_id)
if(NOT generic_cost_count EQUAL 1 OR stable_instance_id EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "identical generic requests did not reuse one stable instance")
endif()

set(structural_generic_source "${work_dir}/structural_generic_instance_reuse.luna")
build_case("${structural_generic_source}" "${work_dir}/structural-generic.moonir")
string(REGEX MATCHALL "\\[generic_instantiation\\]" structural_generic_costs "${last_output}")
list(LENGTH structural_generic_costs structural_generic_cost_count)
file(READ "${work_dir}/structural-generic.moonir" structural_generic_moon)
string(REGEX MATCHALL "identity structural spelling \"(First|Second)\"" structural_type_entries "${structural_generic_moon}")
list(LENGTH structural_type_entries structural_type_entry_count)
if(NOT structural_generic_cost_count EQUAL 1 OR
   NOT structural_type_entry_count EQUAL 1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "equal structural types did not share one TypeId/generic instance\n${structural_generic_moon}")
endif()

file(REMOVE_RECURSE "${work_dir}")
