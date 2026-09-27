cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/package_compiled_fragment_evidence.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_pinned_evidence.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../tests/compiled_fragment_pinned_bundle.cmake")

# Explicit export policy, not inferred from an input manifest or ambient flag.
function(luna_package_compiled_pinned_evidence)
    luna_package_compiled_evidence_impl(pinned)
endfunction()

if(DEFINED LUNA_COMPILED_PINNED_EVIDENCE_OUTPUT_DIR OR CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    luna_package_compiled_pinned_evidence()
endif()
