if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_NATIVE_VERIFIER OR NOT EXISTS "${LUNA_NATIVE_VERIFIER}")
    message(FATAL_ERROR "LUNA_NATIVE_VERIFIER must point at native-artifact-test")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()
if(NOT DEFINED Python3_EXECUTABLE OR NOT EXISTS "${Python3_EXECUTABLE}")
    message(FATAL_ERROR "Python3_EXECUTABLE is required")
endif()
if(NOT DEFINED LUNA_AOT_COMPILER OR NOT EXISTS "${LUNA_AOT_COMPILER}")
    message(FATAL_ERROR "LUNA_AOT_COMPILER is required for the v1 fixture")
endif()

set(work_dir "${LUNA_BINARY_DIR}/native-artifact")
set(package_dir "${work_dir}/native_library")
set(enemy_package_dir "${work_dir}/native_enemy")
set(context_package_dir "${work_dir}/native_context")
set(ref_package_dir "${work_dir}/native_ref")
set(owned_return_package_dir "${work_dir}/native_owned_return")
set(owned_parameter_package_dir "${work_dir}/native_owned_parameter")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${package_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${enemy_package_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${context_package_dir}")
file(APPEND "${context_package_dir}/src/api.luna"
     "\nexport slot checkpoint(value: i32);\n"
     "export runtime fn host_entry() { checkpoint(1) {} }\n")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${ref_package_dir}")
file(APPEND "${ref_package_dir}/src/api.luna"
     "\nexport slot ref_checkpoint(value: i32);\n"
     "export fn host_ref(selected: RuntimeFragmentRef<ref_checkpoint>) -> unit {}\n")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${owned_return_package_dir}")
file(APPEND "${owned_return_package_dir}/src/api.luna"
     "\nexport struct OwnedError { marker: i32; }\n"
     "impl Drop for OwnedError {\n"
     "    fn drop(error: &mut OwnedError) -> unit { error.marker = 0; }\n"
     "}\n"
     "export fn host_owned_error() -> Result<i32, OwnedError> {\n"
     "    let error = new OwnedError(17);\n"
     "    return Err::<i32, OwnedError>(move error);\n"
     "}\n")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${owned_parameter_package_dir}")
file(APPEND "${owned_parameter_package_dir}/src/api.luna"
     "\nexport struct OwnedInput { marker: i32; }\n"
     "impl Drop for OwnedInput {\n"
     "    fn drop(input: &mut OwnedInput) -> unit { input.marker = 0; }\n"
     "}\n"
     "export fn host_owned_input(input: OwnedInput) -> i32 {\n"
     "    return input.marker;\n"
     "}\n")

file(READ "${enemy_package_dir}/src/api.luna" enemy_source)
string(REPLACE "return 42;" "return 13;" enemy_source "${enemy_source}")
file(WRITE "${enemy_package_dir}/src/api.luna" "${enemy_source}")

if(WIN32)
    set(artifact "${package_dir}/build/native/cffi_typed_export.dll")
    set(enemy_artifact "${enemy_package_dir}/build/native/cffi_typed_export.dll")
elseif(APPLE)
    set(artifact "${package_dir}/build/native/libcffi_typed_export.dylib")
    set(enemy_artifact "${enemy_package_dir}/build/native/libcffi_typed_export.dylib")
else()
    set(artifact "${package_dir}/build/native/libcffi_typed_export.so")
    set(enemy_artifact "${enemy_package_dir}/build/native/libcffi_typed_export.so")
endif()
set(ir "${artifact}.ll")
set(trust "${artifact}.trust")
string(REPLACE "${package_dir}" "${context_package_dir}"
       context_artifact "${artifact}")
string(REPLACE "${package_dir}" "${ref_package_dir}"
       ref_artifact "${artifact}")
string(REPLACE "${package_dir}" "${owned_return_package_dir}"
       owned_return_artifact "${artifact}")
string(REPLACE "${package_dir}" "${owned_parameter_package_dir}"
       owned_parameter_artifact "${artifact}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${context_package_dir}" -t native -O2
    RESULT_VARIABLE context_build_result
    OUTPUT_VARIABLE context_build_output
    ERROR_VARIABLE context_build_error)
string(FIND "${context_build_output}\n${context_build_error}"
       "has no runtime-aware public entry ABI" context_gate_diagnostic)
if(context_build_result EQUAL 0 OR context_gate_diagnostic EQUAL -1 OR
   EXISTS "${context_artifact}" OR EXISTS "${context_artifact}.trust")
    message(FATAL_ERROR
        "Native v1 published a context-dependent function without a typed entry ABI.\n"
        "${context_build_output}\n${context_build_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${ref_package_dir}" -t native -O2
    RESULT_VARIABLE ref_build_result
    OUTPUT_VARIABLE ref_build_output
    ERROR_VARIABLE ref_build_error)
string(FIND "${ref_build_output}\n${ref_build_error}"
       "RuntimeFragmentRef source import/dropGlue/wire ABI is not implemented"
       ref_gate_diagnostic)
if(ref_build_result EQUAL 0 OR ref_gate_diagnostic EQUAL -1 OR
   EXISTS "${ref_artifact}" OR EXISTS "${ref_artifact}.trust")
    message(FATAL_ERROR
        "Native v1 published a RuntimeFragmentRef source entry without a typed ABI.\n"
        "${ref_build_output}\n${ref_build_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${owned_return_package_dir}" -t native -O2
    RESULT_VARIABLE owned_return_build_result
    OUTPUT_VARIABLE owned_return_build_output
    ERROR_VARIABLE owned_return_build_error)
string(FIND "${owned_return_build_output}\n${owned_return_build_error}"
       "returns an owned value without a host carrier ABI"
       owned_return_gate_diagnostic)
if(owned_return_build_result EQUAL 0 OR owned_return_gate_diagnostic EQUAL -1 OR
   EXISTS "${owned_return_artifact}" OR EXISTS "${owned_return_artifact}.trust")
    message(FATAL_ERROR
        "Native v1 published an owned Result without a host carrier ABI.\n"
        "${owned_return_build_output}\n${owned_return_build_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${owned_parameter_package_dir}" -t native -O2
    RESULT_VARIABLE owned_parameter_build_result
    OUTPUT_VARIABLE owned_parameter_build_output
    ERROR_VARIABLE owned_parameter_build_error)
string(FIND "${owned_parameter_build_output}\n${owned_parameter_build_error}"
       "accepts a resource parameter without a host carrier ABI"
       owned_parameter_gate_diagnostic)
if(owned_parameter_build_result EQUAL 0 OR
   owned_parameter_gate_diagnostic EQUAL -1 OR
   EXISTS "${owned_parameter_artifact}" OR
   EXISTS "${owned_parameter_artifact}.trust")
    message(FATAL_ERROR
        "Native v1 published an owned parameter without a host carrier ABI.\n"
        "${owned_parameter_build_output}\n${owned_parameter_build_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${package_dir}" -t native -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${artifact}" OR
   NOT EXISTS "${ir}" OR NOT EXISTS "${trust}")
    message(FATAL_ERROR
        "Native library proof build failed.\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${enemy_package_dir}" -t native -O2
    RESULT_VARIABLE enemy_build_result
    OUTPUT_VARIABLE enemy_build_output
    ERROR_VARIABLE enemy_build_error)
if(NOT enemy_build_result EQUAL 0 OR NOT EXISTS "${enemy_artifact}")
    message(FATAL_ERROR
        "Native TOCTOU replacement build failed.\n"
        "${enemy_build_output}\n${enemy_build_error}")
endif()

file(READ "${ir}" ir_text)
string(FIND "${ir_text}" "@luna_native_proof_v1" proof_symbol)
string(FIND "${ir_text}" "@luna_native_library_descriptor_v1" descriptor_query)
string(FIND "${ir_text}" "@luna_native_library_descriptor_v2" typed_descriptor_query)
string(FIND "${ir_text}" "@luna_native_library_descriptor_v3" owned_descriptor_query)
if(WIN32)
    string(FIND "${ir_text}" "section \".luna$proof\"" proof_section)
    string(FIND "${ir_text}" "section \".luna$desc\"" descriptor_section)
    string(FIND "${ir_text}" "section \".luna$desc2\"" typed_descriptor_section)
elseif(APPLE)
    string(FIND "${ir_text}" "section \"__DATA,__luna_proof\"" proof_section)
    string(FIND "${ir_text}" "section \"__DATA,__luna_desc\"" descriptor_section)
    string(FIND "${ir_text}" "section \"__DATA,__luna_desc2\"" typed_descriptor_section)
else()
    string(FIND "${ir_text}" "section \".luna.native.proof\"" proof_section)
    string(FIND "${ir_text}" "section \".luna.native.descriptor\"" descriptor_section)
    string(FIND "${ir_text}" "section \".luna.native.descriptor.v2\"" typed_descriptor_section)
endif()
if(proof_symbol EQUAL -1 OR proof_section EQUAL -1 OR
   descriptor_query EQUAL -1 OR descriptor_section EQUAL -1 OR
   typed_descriptor_query EQUAL -1 OR typed_descriptor_section EQUAL -1 OR
   NOT owned_descriptor_query EQUAL -1)
    message(FATAL_ERROR
        "Native proof/typed registry is missing or candidate v3 was emitted")
endif()

execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${artifact}" "${trust}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR
        "sealed Native artifact failed explicit trust verification.\n"
        "${verify_output}\n${verify_error}")
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${LUNA_SOURCE_DIR}/tests/native_artifact_oracle.py"
        "${artifact}" "${trust}"
    RESULT_VARIABLE oracle_result
    OUTPUT_VARIABLE oracle_output
    ERROR_VARIABLE oracle_error)
if(NOT oracle_result EQUAL 0)
    message(FATAL_ERROR
        "independent Native proof oracle failed.\n${oracle_output}\n${oracle_error}")
endif()

# This separate C library implements only descriptor/proof v1. Its proof is
# prepared and sealed after the platform compiler links it, so it exercises
# compatibility with an independently produced artifact rather than a renamed
# query inside the current Luna-generated v2 image.
if(WIN32)
    set(legacy_artifact "${work_dir}/independent-v1.dll")
elseif(APPLE)
    set(legacy_artifact "${work_dir}/libindependent-v1.dylib")
else()
    set(legacy_artifact "${work_dir}/libindependent-v1.so")
endif()
set(legacy_trust "${legacy_artifact}.trust")
set(legacy_link_mode -shared)
if(APPLE)
    set(legacy_link_mode -dynamiclib)
endif()
execute_process(
    COMMAND "${LUNA_AOT_COMPILER}" -x c -std=c11 -fPIC
        ${legacy_link_mode}
        -I "${LUNA_SOURCE_DIR}/src"
        "${LUNA_SOURCE_DIR}/tests/fixtures/native_v1_artifact_fixture.c"
        -o "${legacy_artifact}"
    RESULT_VARIABLE legacy_compile_result
    OUTPUT_VARIABLE legacy_compile_output
    ERROR_VARIABLE legacy_compile_error)
if(NOT legacy_compile_result EQUAL 0 OR NOT EXISTS "${legacy_artifact}")
    message(FATAL_ERROR "independent Native v1 fixture did not link.\n"
        "${legacy_compile_output}\n${legacy_compile_error}")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --prepare-legacy
        "${legacy_artifact}" "${legacy_trust}"
    RESULT_VARIABLE legacy_seal_result
    ERROR_VARIABLE legacy_seal_error)
if(NOT legacy_seal_result EQUAL 0 OR NOT EXISTS "${legacy_trust}")
    message(FATAL_ERROR "independent Native v1 fixture did not seal.\n"
        "${legacy_seal_error}")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${legacy_artifact}" "${legacy_trust}"
    RESULT_VARIABLE legacy_verify_result
    ERROR_VARIABLE legacy_verify_error)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${LUNA_SOURCE_DIR}/tests/native_artifact_oracle.py"
        "${legacy_artifact}" "${legacy_trust}"
    RESULT_VARIABLE legacy_oracle_result
    ERROR_VARIABLE legacy_oracle_error)
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --load-call
        "${legacy_artifact}" "${legacy_trust}"
        "symbol:legacy-answer" "contract:legacy-v1"
    RESULT_VARIABLE legacy_call_result
    OUTPUT_VARIABLE legacy_call_output
    ERROR_VARIABLE legacy_call_error)
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --load-legacy-generation
        "${legacy_artifact}" "${legacy_trust}"
        "symbol:legacy-answer" "contract:legacy-v1"
    RESULT_VARIABLE legacy_generation_result
    OUTPUT_VARIABLE legacy_generation_output
    ERROR_VARIABLE legacy_generation_error)
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --load-typed-call
        "${legacy_artifact}" "${legacy_trust}"
        "symbol:legacy-answer" "contract:legacy-v1"
    RESULT_VARIABLE legacy_typed_result
    ERROR_VARIABLE legacy_typed_error)
string(FIND "${legacy_typed_error}" "no v2 typed descriptor"
       legacy_typed_diagnostic)
if(NOT legacy_verify_result EQUAL 0 OR NOT legacy_oracle_result EQUAL 0 OR
   NOT legacy_call_result EQUAL 0 OR NOT legacy_call_output STREQUAL "7\n" OR
   NOT legacy_generation_result EQUAL 0 OR
   NOT legacy_generation_output STREQUAL "v1-only\n" OR
   legacy_typed_result EQUAL 0 OR legacy_typed_diagnostic EQUAL -1)
    message(FATAL_ERROR
        "independent Native v1 compatibility fixture failed.\n"
        "verify: ${legacy_verify_error}\noracle: ${legacy_oracle_error}\n"
        "call: ${legacy_call_output} ${legacy_call_error}\n"
        "generation: ${legacy_generation_output} ${legacy_generation_error}\n"
        "typed: ${legacy_typed_error}")
endif()

# A separately sealed v1 producer may supply byte strings that match its own
# proof digest while violating the candidate descriptor's UTF-8 contract.
if(WIN32)
    set(invalid_utf8_artifact "${work_dir}/invalid-utf8-v1.dll")
elseif(APPLE)
    set(invalid_utf8_artifact "${work_dir}/libinvalid-utf8-v1.dylib")
else()
    set(invalid_utf8_artifact "${work_dir}/libinvalid-utf8-v1.so")
endif()
set(invalid_utf8_trust "${invalid_utf8_artifact}.trust")
execute_process(
    COMMAND "${LUNA_AOT_COMPILER}" -x c -std=c11 -fPIC
        -DLUNA_TEST_INVALID_UTF8 ${legacy_link_mode}
        -I "${LUNA_SOURCE_DIR}/src"
        "${LUNA_SOURCE_DIR}/tests/fixtures/native_v1_artifact_fixture.c"
        -o "${invalid_utf8_artifact}"
    RESULT_VARIABLE invalid_utf8_compile_result
    ERROR_VARIABLE invalid_utf8_compile_error)
if(NOT invalid_utf8_compile_result EQUAL 0)
    message(FATAL_ERROR "invalid UTF-8 Native fixture did not link.\n"
        "${invalid_utf8_compile_error}")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --prepare-legacy-invalid-utf8
        "${invalid_utf8_artifact}" "${invalid_utf8_trust}"
    RESULT_VARIABLE invalid_utf8_seal_result
    ERROR_VARIABLE invalid_utf8_seal_error)
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}"
        "${invalid_utf8_artifact}" "${invalid_utf8_trust}"
    RESULT_VARIABLE invalid_utf8_verify_result
    ERROR_VARIABLE invalid_utf8_verify_error)
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --load-only
        "${invalid_utf8_artifact}" "${invalid_utf8_trust}"
    RESULT_VARIABLE invalid_utf8_load_result
    ERROR_VARIABLE invalid_utf8_load_error)
string(FIND "${invalid_utf8_load_error}" "invalid export row"
       invalid_utf8_load_diagnostic)
if(NOT invalid_utf8_seal_result EQUAL 0 OR
   NOT invalid_utf8_verify_result EQUAL 0 OR
   invalid_utf8_load_result EQUAL 0 OR
   invalid_utf8_load_diagnostic EQUAL -1)
    message(FATAL_ERROR
        "sealed Native descriptor accepted invalid UTF-8.\n"
        "seal: ${invalid_utf8_seal_error}\n"
        "verify: ${invalid_utf8_verify_error}\n"
        "load: ${invalid_utf8_load_error}")
endif()

# Compile the parallel v2 query in C, patch only its canonical row digest,
# then seal each linked image independently. This exercises a producer other
# than Luna's LLVM descriptor emitter.
foreach(v2_variant IN ITEMS valid bad-row-size)
    if(WIN32)
        set(v2_artifact "${work_dir}/independent-v2-${v2_variant}.dll")
    elseif(APPLE)
        set(v2_artifact "${work_dir}/libindependent-v2-${v2_variant}.dylib")
    else()
        set(v2_artifact "${work_dir}/libindependent-v2-${v2_variant}.so")
    endif()
    set(v2_trust "${v2_artifact}.trust")
    set(v2_defines -DLUNA_TEST_V2_DESCRIPTOR)
    if(v2_variant STREQUAL "bad-row-size")
        list(APPEND v2_defines -DLUNA_TEST_V2_BAD_ROW_SIZE)
    endif()
    execute_process(
        COMMAND "${LUNA_AOT_COMPILER}" -x c -std=c11 -fPIC
            ${v2_defines} ${legacy_link_mode}
            -I "${LUNA_SOURCE_DIR}/src"
            "${LUNA_SOURCE_DIR}/tests/fixtures/native_v1_artifact_fixture.c"
            -o "${v2_artifact}"
        RESULT_VARIABLE v2_compile_result
        ERROR_VARIABLE v2_compile_error)
    if(NOT v2_compile_result EQUAL 0 OR NOT EXISTS "${v2_artifact}")
        message(FATAL_ERROR "independent Native v2 fixture did not link.\n"
            "${v2_compile_error}")
    endif()
    execute_process(
        COMMAND "${LUNA_NATIVE_VERIFIER}" --prepare-v2-fixture
            "${v2_artifact}" "${v2_trust}"
        RESULT_VARIABLE v2_seal_result
        ERROR_VARIABLE v2_seal_error)
    if(NOT v2_seal_result EQUAL 0 OR NOT EXISTS "${v2_trust}")
        message(FATAL_ERROR "independent Native v2 fixture did not seal.\n"
            "${v2_seal_error}")
    endif()
    execute_process(
        COMMAND "${LUNA_NATIVE_VERIFIER}" "${v2_artifact}" "${v2_trust}"
        RESULT_VARIABLE v2_verify_result
        ERROR_VARIABLE v2_verify_error)
    if(NOT v2_verify_result EQUAL 0)
        message(FATAL_ERROR "independent Native v2 proof did not verify.\n"
            "${v2_verify_error}")
    endif()
    if(v2_variant STREQUAL "valid")
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --load-typed-call
                "${v2_artifact}" "${v2_trust}"
                "symbol:legacy-answer" "contract:legacy-v1"
            RESULT_VARIABLE v2_load_result
            OUTPUT_VARIABLE v2_load_output
            ERROR_VARIABLE v2_load_error)
        if(NOT v2_load_result EQUAL 0 OR NOT v2_load_output STREQUAL "7\n")
            message(FATAL_ERROR "independent Native v2 typed call failed.\n"
                "${v2_load_output}\n${v2_load_error}")
        endif()
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --pinned-binding-outlives-runtime
                "${v2_artifact}" "${v2_trust}"
                "symbol:legacy-answer" "contract:legacy-v1"
            RESULT_VARIABLE v2_pinned_result
            OUTPUT_VARIABLE v2_pinned_output
            ERROR_VARIABLE v2_pinned_error)
        if(NOT v2_pinned_result EQUAL 0 OR
           NOT v2_pinned_output STREQUAL "pinned-v2\n")
            message(FATAL_ERROR
                "independent Native v2 pinned binding lost its lease.\n"
                "${v2_pinned_output}\n${v2_pinned_error}")
        endif()
    else()
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --load-only
                "${v2_artifact}" "${v2_trust}"
            RESULT_VARIABLE v2_load_result
            ERROR_VARIABLE v2_load_error)
        string(FIND "${v2_load_error}" "invalid export row"
               v2_load_diagnostic)
        if(v2_load_result EQUAL 0 OR v2_load_diagnostic EQUAL -1)
            message(FATAL_ERROR
                "independent Native v2 invalid row size was accepted.\n"
                "${v2_load_error}")
        endif()
    endif()
endforeach()

# An independently linked v3 candidate carries Ref/Result metadata but is
# validation-only until the generated host carrier and status path exist.
set(v3_variants valid bad-row-size bad-profile bad-slot-digest bad-identity
    bad-entry-pointer bad-linkage)
if(NOT WIN32)
    list(APPEND v3_variants dependency-entry)
endif()
foreach(v3_variant IN LISTS v3_variants)
    if(WIN32)
        set(v3_artifact "${work_dir}/independent-v3-${v3_variant}.dll")
    elseif(APPLE)
        set(v3_artifact "${work_dir}/libindependent-v3-${v3_variant}.dylib")
    else()
        set(v3_artifact "${work_dir}/libindependent-v3-${v3_variant}.so")
    endif()
    set(v3_trust "${v3_artifact}.trust")
    set(v3_defines -DLUNA_TEST_V3_DESCRIPTOR)
    if(v3_variant STREQUAL "bad-row-size")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_ROW_SIZE)
    elseif(v3_variant STREQUAL "bad-profile")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_PROFILE)
    elseif(v3_variant STREQUAL "bad-slot-digest")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_SLOT_DIGEST)
    elseif(v3_variant STREQUAL "bad-identity")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_IDENTITY)
    elseif(v3_variant STREQUAL "bad-entry-pointer")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_ENTRY_POINTER)
    elseif(v3_variant STREQUAL "bad-linkage")
        list(APPEND v3_defines -DLUNA_TEST_V3_BAD_LINKAGE)
    elseif(v3_variant STREQUAL "dependency-entry")
        list(APPEND v3_defines -DLUNA_TEST_V3_DEPENDENCY_ENTRY)
    endif()
    execute_process(
        COMMAND "${LUNA_AOT_COMPILER}" -x c -std=c11 -fPIC
            ${v3_defines} ${legacy_link_mode}
            -I "${LUNA_SOURCE_DIR}/src"
            "${LUNA_SOURCE_DIR}/tests/fixtures/native_v1_artifact_fixture.c"
            -o "${v3_artifact}"
        RESULT_VARIABLE v3_compile_result
        ERROR_VARIABLE v3_compile_error)
    if(NOT v3_compile_result EQUAL 0 OR NOT EXISTS "${v3_artifact}")
        message(FATAL_ERROR "independent Native v3 fixture did not link.\n"
            "${v3_compile_error}")
    endif()
    set(v3_prepare_mode --prepare-v3-fixture)
    if(v3_variant STREQUAL "dependency-entry")
        set(v3_prepare_mode --prepare-v3-dependency-fixture)
    endif()
    execute_process(
        COMMAND "${LUNA_NATIVE_VERIFIER}" ${v3_prepare_mode}
            "${v3_artifact}" "${v3_trust}"
        RESULT_VARIABLE v3_seal_result
        ERROR_VARIABLE v3_seal_error)
    if(NOT v3_seal_result EQUAL 0 OR NOT EXISTS "${v3_trust}")
        message(FATAL_ERROR "independent Native v3 fixture did not seal.\n"
            "${v3_seal_error}")
    endif()
    execute_process(
        COMMAND "${LUNA_NATIVE_VERIFIER}" "${v3_artifact}" "${v3_trust}"
        RESULT_VARIABLE v3_verify_result
        ERROR_VARIABLE v3_verify_error)
    if(NOT v3_verify_result EQUAL 0)
        message(FATAL_ERROR "independent Native v3 proof did not verify.\n"
            "${v3_verify_error}")
    endif()
    execute_process(
        COMMAND "${LUNA_NATIVE_VERIFIER}" --load-only
            "${v3_artifact}" "${v3_trust}"
        RESULT_VARIABLE v3_load_result
        ERROR_VARIABLE v3_load_error)
    if(v3_variant STREQUAL "valid")
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --load-call
                "${v3_artifact}" "${v3_trust}"
                "symbol:legacy-answer" "contract:legacy-v1"
            RESULT_VARIABLE v3_legacy_call_result
            OUTPUT_VARIABLE v3_legacy_call_output
            ERROR_VARIABLE v3_legacy_call_error)
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --inspect-v3-candidate
                "${v3_artifact}" "${v3_trust}"
            RESULT_VARIABLE v3_candidate_result
            OUTPUT_VARIABLE v3_candidate_output
            ERROR_VARIABLE v3_candidate_error)
        execute_process(
            COMMAND "${LUNA_NATIVE_VERIFIER}" --load-legacy-generation
                "${v3_artifact}" "${v3_trust}"
                "symbol:legacy-answer" "contract:legacy-v1"
            RESULT_VARIABLE v3_generation_result
            OUTPUT_VARIABLE v3_generation_output
            ERROR_VARIABLE v3_generation_error)
        if(NOT v3_load_result EQUAL 0 OR
           NOT v3_legacy_call_result EQUAL 0 OR
           NOT v3_legacy_call_output STREQUAL "7\n" OR
           NOT v3_candidate_result EQUAL 0 OR
           NOT v3_candidate_output STREQUAL "v3-metadata-only\n" OR
           NOT v3_generation_result EQUAL 0 OR
           NOT v3_generation_output STREQUAL "v1-only\n")
            message(FATAL_ERROR
                "independent v3 metadata escaped the unprofiled generation gate.\n"
                "load: ${v3_load_error}\n"
                "legacy call: ${v3_legacy_call_error}\n"
                "candidate: ${v3_candidate_error}\n"
                "generation: ${v3_generation_error}")
        endif()
    else()
        if(v3_variant STREQUAL "bad-row-size" OR
           v3_variant STREQUAL "bad-profile")
            set(v3_expected "invalid export row")
        elseif(v3_variant STREQUAL "bad-slot-digest" OR
               v3_variant STREQUAL "bad-identity")
            set(v3_expected "rows do not match their descriptor digest")
        elseif(v3_variant STREQUAL "bad-linkage")
            set(v3_expected "entry does not match its resolved symbol")
        elseif(v3_variant STREQUAL "dependency-entry")
            set(v3_expected "entry is outside its verified image")
        else()
            set(v3_expected "invalid export row")
        endif()
        string(FIND "${v3_load_error}" "${v3_expected}"
               v3_load_diagnostic)
        if(v3_load_result EQUAL 0 OR v3_load_diagnostic EQUAL -1)
            message(FATAL_ERROR
                "independent Native v3 ${v3_variant} row was accepted.\n"
                "${v3_load_error}")
        endif()
    endif()
endforeach()

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${LUNA_SOURCE_DIR}/tests/native_artifact_consumer.py"
        "${artifact}" "${trust}" "${LUNA_NATIVE_VERIFIER}"
        "${enemy_artifact}" "${enemy_artifact}.trust"
    RESULT_VARIABLE consumer_result
    OUTPUT_VARIABLE consumer_output
    ERROR_VARIABLE consumer_error)
if(NOT consumer_result EQUAL 0)
    message(FATAL_ERROR
        "independent Native library consumer failed (exit ${consumer_result}).\n"
        "${consumer_output}\n${consumer_error}")
endif()
foreach(expected IN ITEMS
        "org.luna.fixture.cffi_typed_export"
        "1.0.0"
        "luna/0.3.0@")
    string(FIND "${verify_output}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "verified proof omitted '${expected}'")
    endif()
endforeach()

set(tampered "${work_dir}/tampered-native-library")
file(COPY_FILE "${artifact}" "${tampered}")
file(APPEND "${tampered}" "tamper")
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${tampered}" "${trust}"
    RESULT_VARIABLE tampered_result
    ERROR_VARIABLE tampered_error)
string(FIND "${tampered_error}" "digest does not match" tampered_diagnostic)
if(tampered_result EQUAL 0 OR tampered_diagnostic EQUAL -1)
    message(FATAL_ERROR "tampered Native artifact was not rejected fail-closed")
endif()

set(proof_tampered "${work_dir}/proof-tampered-native-library")
set(forged_trust "${work_dir}/forged-export.trust")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${LUNA_SOURCE_DIR}/tests/native_artifact_mutate.py"
        "${artifact}" "${proof_tampered}" export "${trust}" "${forged_trust}"
    RESULT_VARIABLE proof_mutation_result)
if(NOT proof_mutation_result EQUAL 0)
    message(FATAL_ERROR "could not create deterministic proof mutation")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${proof_tampered}" "${trust}"
    RESULT_VARIABLE proof_tampered_result
    ERROR_VARIABLE proof_tampered_error)
string(FIND "${proof_tampered_error}"
       "not present in the explicit trust store" proof_tampered_diagnostic)
if(proof_tampered_result EQUAL 0 OR proof_tampered_diagnostic EQUAL -1)
    message(FATAL_ERROR "mutated proof descriptor bypassed explicit trust")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${proof_tampered}" "${forged_trust}"
    RESULT_VARIABLE forged_offline_result
    ERROR_VARIABLE forged_offline_error)
if(NOT forged_offline_result EQUAL 0)
    message(FATAL_ERROR
        "forged export trust precondition did not pass offline verification.\n"
        "${forged_offline_error}")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" --load-only
        "${proof_tampered}" "${forged_trust}"
    RESULT_VARIABLE forged_load_result
    ERROR_VARIABLE forged_load_error)
string(FIND "${forged_load_error}"
       "does not match its proof export digest" forged_load_diagnostic)
if(forged_load_result EQUAL 0 OR forged_load_diagnostic EQUAL -1)
    message(FATAL_ERROR
        "proof-bound descriptor validation accepted a forged export registry")
endif()

set(dependency_tampered "${work_dir}/dependency-tampered-native-library")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${LUNA_SOURCE_DIR}/tests/native_artifact_mutate.py"
        "${artifact}" "${dependency_tampered}" dependency
    RESULT_VARIABLE dependency_mutation_result)
if(NOT dependency_mutation_result EQUAL 0)
    message(FATAL_ERROR "could not create dependency-proof mutation")
endif()
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${dependency_tampered}" "${trust}"
    RESULT_VARIABLE dependency_tampered_result
    ERROR_VARIABLE dependency_tampered_error)
string(FIND "${dependency_tampered_error}"
       "does not match the final dynamic dependency table"
       dependency_tampered_diagnostic)
if(dependency_tampered_result EQUAL 0 OR
   dependency_tampered_diagnostic EQUAL -1)
    message(FATAL_ERROR "forged final dependency proof was accepted")
endif()

set(empty_trust "${work_dir}/empty.trust")
file(WRITE "${empty_trust}" "")
execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${artifact}" "${empty_trust}"
    RESULT_VARIABLE untrusted_result
    ERROR_VARIABLE untrusted_error)
string(FIND "${untrusted_error}" "not present in the explicit trust store"
       untrusted_diagnostic)
if(untrusted_result EQUAL 0 OR untrusted_diagnostic EQUAL -1)
    message(FATAL_ERROR "self-asserted Native proof bypassed explicit trust")
endif()

execute_process(
    COMMAND "${LUNA_NATIVE_VERIFIER}" "${LUNA_EXECUTABLE}" "${trust}"
    RESULT_VARIABLE missing_result
    ERROR_VARIABLE missing_error)
string(FIND "${missing_error}" "no valid embedded Luna proof" missing_diagnostic)
if(missing_result EQUAL 0 OR missing_diagnostic EQUAL -1)
    message(FATAL_ERROR "proof-free native binary was accepted")
endif()

file(REMOVE_RECURSE "${work_dir}")
