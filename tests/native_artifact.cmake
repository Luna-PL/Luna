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

set(work_dir "${LUNA_BINARY_DIR}/native-artifact")
set(package_dir "${work_dir}/native_library")
set(enemy_package_dir "${work_dir}/native_enemy")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${package_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/cffi_typed_export"
     DESTINATION "${work_dir}")
file(RENAME "${work_dir}/cffi_typed_export" "${enemy_package_dir}")

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
if(WIN32)
    string(FIND "${ir_text}" "section \".luna$proof\"" proof_section)
    string(FIND "${ir_text}" "section \".luna$desc\"" descriptor_section)
elseif(APPLE)
    string(FIND "${ir_text}" "section \"__DATA,__luna_proof\"" proof_section)
    string(FIND "${ir_text}" "section \"__DATA,__luna_desc\"" descriptor_section)
else()
    string(FIND "${ir_text}" "section \".luna.native.proof\"" proof_section)
    string(FIND "${ir_text}" "section \".luna.native.descriptor\"" descriptor_section)
endif()
if(proof_symbol EQUAL -1 OR proof_section EQUAL -1 OR
   descriptor_query EQUAL -1 OR descriptor_section EQUAL -1)
    message(FATAL_ERROR
        "Native proof or typed descriptor registry is missing from emitted IR")
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
        "independent Native library consumer failed.\n"
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
