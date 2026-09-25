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
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/runtime_retention_descriptor.luna"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/exported_fragment_runtime.luna"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/runtime_slot_cost.luna"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/static_slot_cost.luna"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/selector_user_logic.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/concepts.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/generic_instance_reuse.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/structural_generic_instance_reuse.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/anonymous_records.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/anonymous_record_owned_field.luna" DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/usage_blocks.luna" DESTINATION "${work_dir}")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")

function(build_case source moon_file)
    get_filename_component(case_name "${source}" NAME_WE)
    luna_stage_aot_application("${source}" "moon_cost_${case_name}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O0
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
    set(last_ir_path "${LUNA_AOT_IR_PATH}" PARENT_SCOPE)
endfunction()

set(kernel_source "${work_dir}/kernel_unused.luna")
build_case("${kernel_source}" "${work_dir}/kernel-default.moonir")
set(kernel_ir "${last_ir_path}")
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
string(FIND "${reserved_ir}" "void @unused_kernel" reserved_kernel)
string(FIND "${reserved_moon}" "kernel_runtime_reserved" reserved_feature)
if(reserved_gpu EQUAL -1 OR reserved_kernel EQUAL -1 OR reserved_feature EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "explicit kernel reservation did not materialize its audited cost")
endif()

set(runtime_source "${work_dir}/runtime_retention_descriptor.luna")
build_case("${runtime_source}" "${work_dir}/runtime-retention.moonir")
file(READ "${last_ir_path}" runtime_ir)
file(READ "${work_dir}/runtime-retention.moonir" runtime_moon)
string(FIND "${runtime_ir}" "__moon_runtime_registry_" runtime_registry)
string(FIND "${runtime_ir}" "moon.runtime.declaration.v1" runtime_descriptor_v1)
string(FIND "${runtime_ir}" "moon.runtime.registry.v1" runtime_registry_v1)
string(FIND "${runtime_ir}" "symbol_" runtime_symbol_id)
string(FIND "${runtime_ir}" "contract_" runtime_contract_id)
string(FIND "${runtime_ir}" "type_" runtime_type_id)
string(REGEX MATCHALL
    "@__moon_descriptor_[^\n]*ptr null \\}, section"
    runtime_identity_only_descriptors "${runtime_ir}")
list(LENGTH runtime_identity_only_descriptors runtime_identity_only_count)
string(FIND "${runtime_ir}" "dynamic.select.unique" dynamic_binding)
string(FIND "${runtime_ir}" "rt_gpu_" dynamic_gpu)
string(FIND "${runtime_moon}" "dynamic_select" dynamic_feature)
if(runtime_registry EQUAL -1 OR runtime_descriptor_v1 EQUAL -1 OR
   runtime_registry_v1 EQUAL -1 OR runtime_symbol_id EQUAL -1 OR
   runtime_contract_id EQUAL -1 OR runtime_type_id EQUAL -1 OR
   NOT runtime_identity_only_count EQUAL 2 OR
   NOT dynamic_binding EQUAL -1 OR
   NOT dynamic_gpu EQUAL -1 OR NOT dynamic_feature EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "runtime descriptors retained the removed dynamic-select cost")
endif()

set(runtime_fragment_source "${work_dir}/exported_fragment_runtime.luna")
build_case("${runtime_fragment_source}"
           "${work_dir}/exported-fragment-runtime.moonir")
file(READ "${last_ir_path}" runtime_fragment_ir)
foreach(required IN ITEMS
        "moon.runtime.fragment.v1"
        "__luna_runtime_fragment_entry"
        "__moon_fragment_factory_"
        "__moon_fragment_destroy_"
        "__moon_fragment_execute_"
        "luna_runtime_fragment_activation_arguments_v1"
        "luna_runtime_fragment_activation_resume_v1"
        "fragment.resume.escaped")
    string(FIND "${runtime_fragment_ir}" "${required}" found)
    if(found EQUAL -1)
        file(REMOVE_RECURSE "${work_dir}")
        message(FATAL_ERROR
            "exported Fragment did not materialize audited runtime cost '${required}'")
    endif()
endforeach()

# Count generated call sites, rather than timing execution. The dynamic entry
# pays for one explicit dispatch and a context-aware ABI; a static composition
# must not acquire either the dispatch or the Runtime catalog machinery.
set(dynamic_slot_source "${work_dir}/runtime_slot_cost.luna")
build_case("${dynamic_slot_source}" "${work_dir}/runtime-slot-cost.moonir")
file(READ "${last_ir_path}" dynamic_slot_ir)
string(REGEX MATCHALL
    "call i32 @luna_runtime_fragment_dispatch_v1\\("
    dynamic_slot_dispatch_calls "${dynamic_slot_ir}")
list(LENGTH dynamic_slot_dispatch_calls dynamic_slot_dispatch_count)
string(FIND "${dynamic_slot_ir}" "fragment.context" dynamic_slot_context)
string(FIND "${dynamic_slot_ir}" "__moon_runtime_registry_" dynamic_slot_registry)
string(FIND "${dynamic_slot_ir}" "runtime.slot.frame" dynamic_slot_frame)
foreach(forbidden IN ITEMS
        "snapshotRuntimeFragmentCandidates"
        "makeRuntimeFragmentBindingSet"
        "pinFragmentBindings")
    string(FIND "${dynamic_slot_ir}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR
            "dynamic Slot hot path contains host policy '${forbidden}'")
    endif()
endforeach()
if(NOT dynamic_slot_dispatch_count EQUAL 1 OR
   dynamic_slot_context EQUAL -1 OR
   dynamic_slot_registry EQUAL -1 OR
   dynamic_slot_frame EQUAL -1)
    message(FATAL_ERROR
        "dynamic Slot lost its single explicit dispatch/context boundary")
endif()

set(static_slot_source "${work_dir}/static_slot_cost.luna")
build_case("${static_slot_source}" "${work_dir}/static-slot-cost.moonir")
file(READ "${last_ir_path}" static_slot_ir)
foreach(forbidden IN ITEMS
        "luna_runtime_fragment_dispatch_v1"
        "__moon_runtime_registry_"
        "runtime.slot.frame"
        "fragment.context")
    string(FIND "${static_slot_ir}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR
            "static Slot composition retained runtime machinery '${forbidden}'")
    endif()
endforeach()

set(static_selector_source "${work_dir}/selector_user_logic.luna")
build_case("${static_selector_source}" "${work_dir}/static-selector.moonir")
file(READ "${last_ir_path}" static_selector_ir)
file(READ "${work_dir}/static-selector.moonir" static_selector_moon)
string(FIND "${static_selector_ir}" "choose_release" static_selector_machine_code)
string(FIND "${static_selector_ir}" "__moon_runtime_registry_" static_selector_registry)
string(FIND "${static_selector_ir}" "moon.runtime.declaration.v1" static_descriptor_v1)
string(FIND "${static_selector_moon}" "choose_release" static_selector_protocol)
string(FIND "${static_selector_moon}" "declaration_view" static_selector_view)
string(FIND "${static_selector_moon}" "dynamic_select" static_selector_dynamic)
if(NOT static_selector_machine_code EQUAL -1 OR
   NOT static_selector_registry EQUAL -1 OR
   NOT static_descriptor_v1 EQUAL -1 OR
   NOT static_selector_protocol EQUAL -1 OR
   NOT static_selector_view EQUAL -1 OR
   NOT static_selector_dynamic EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "static selector/view protocol was not fully erased\n${static_selector_moon}")
endif()

set(concept_source "${work_dir}/concepts.luna")
build_case("${concept_source}" "${work_dir}/concepts.moonir")
file(READ "${last_ir_path}" concept_ir)
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
string(REGEX MATCHALL "identity nominal spelling \"(First|Second)\"" nominal_type_entries "${structural_generic_moon}")
list(LENGTH nominal_type_entries nominal_type_entry_count)
if(NOT structural_generic_cost_count EQUAL 2 OR
   NOT nominal_type_entry_count EQUAL 2)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "distinct named types did not retain distinct TypeIds/generic instances\n${structural_generic_moon}")
endif()

set(copy_record_source "${work_dir}/anonymous_records.luna")
build_case("${copy_record_source}" "${work_dir}/copy-record.moonir")
file(READ "${last_ir_path}" copy_record_ir)
file(READ "${work_dir}/copy-record.moonir" copy_record_moon)
string(FIND "${copy_record_ir}" "rt_dealloc" copy_record_cleanup)
string(FIND "${copy_record_moon}" "Cartesian" copy_record_constraint)
if(NOT copy_record_cleanup EQUAL -1 OR
   NOT copy_record_constraint EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "Copy-only record or erased constraint paid a runtime cost\n${copy_record_ir}\n${copy_record_moon}")
endif()

set(owned_record_source "${work_dir}/anonymous_record_owned_field.luna")
build_case("${owned_record_source}" "${work_dir}/owned-record.moonir")
file(READ "${last_ir_path}" owned_record_ir)
string(FIND "${owned_record_ir}" "rt_dealloc" owned_record_cleanup)
if(owned_record_cleanup EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "record with an owned field lost its recursive cleanup")
endif()

set(usage_block_source "${work_dir}/usage_blocks.luna")
build_case("${usage_block_source}" "${work_dir}/usage-blocks.moonir")
file(READ "${last_ir_path}" usage_block_ir)
file(READ "${work_dir}/usage-blocks.moonir" usage_block_moon)
string(FIND "${usage_block_ir}" "usage_scope" usage_scope_llvm)
string(FIND "${usage_block_moon}" "usage_scope" usage_scope_moon)
if(NOT usage_scope_llvm EQUAL -1 OR
   NOT usage_scope_moon EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "usage-block sugar leaked a scope construct into generated artifacts")
endif()

file(REMOVE_RECURSE "${work_dir}")
