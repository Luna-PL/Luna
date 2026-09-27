cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_evidence.cmake")

# Byte/inventory checks only. Never executes archived validation code or probes.
function(luna_verify_compiled_pinned_evidence directory)
    luna_verify_compiled_evidence_impl("${directory}" pinned)
endfunction()

if(DEFINED LUNA_COMPILED_PINNED_EVIDENCE_DIR)
    luna_verify_compiled_pinned_evidence("${LUNA_COMPILED_PINNED_EVIDENCE_DIR}")
elseif(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    message(FATAL_ERROR "LUNA_COMPILED_PINNED_EVIDENCE_DIR is required")
endif()
